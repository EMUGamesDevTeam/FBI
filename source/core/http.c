#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>
#include <curl/curl.h>
#include <jansson.h>
#include <zlib.h>

#include "fs.h"
#include "../ui/error.h"
#include "http.h"
#include "stringutil.h"

#define MAKE_HTTP_USER_AGENT_(major, minor, micro) ("Mozilla/5.0 (Nintendo 3DS; Mobile; rv:10.0) Gecko/20100101 FBI/" #major "." #minor "." #micro)
#define MAKE_HTTP_USER_AGENT(major, minor, micro) MAKE_HTTP_USER_AGENT_(major, minor, micro)
#define HTTP_USER_AGENT MAKE_HTTP_USER_AGENT(VERSION_MAJOR, VERSION_MINOR, VERSION_MICRO)

#define HTTP_TIMEOUT_SEC 15
#define HTTP_TIMEOUT_NS ((u64) HTTP_TIMEOUT_SEC * 1000000000)

struct http_context_s {
    httpcContext httpc;

    bool compressed;
    z_stream inflate;
    u8 buffer[32 * 1024];
    u32 bufferSize;
};

void http_resolve_redirect(char* oldUrl, const char* redirectTo, size_t size) {
    if(size > 0) {
        if(redirectTo[0] == '/') {
            char* baseEnd = oldUrl;

            // Find the third slash to find the end of the URL's base; e.g. https://www.example.com/
            u32 slashCount = 0;
            while(*baseEnd != '\0' && (baseEnd = strchr(baseEnd + 1, '/')) != NULL) {
                slashCount++;
                if(slashCount == 3) {
                    break;
                }
            }

            // If there are less than 3 slashes, assume the base URL ends at the end of the string; e.g. https://www.example.com
            if(slashCount != 3) {
                baseEnd = oldUrl + strlen(oldUrl);
            }

            size_t baseLen = baseEnd - oldUrl;
            if(baseLen < size) {
                string_copy(baseEnd, redirectTo, size - baseLen);
            }
        } else {
            string_copy(oldUrl, redirectTo, size);
        }
    }
}

Result http_open(http_context* context, const char* url, bool userAgent) {
    return http_open_ranged(context, url, userAgent, 0, 0);
}

Result http_open_ranged(http_context* context, const char* url, bool userAgent, u32 rangeStart, u32 rangeEnd) {
    if(url == NULL) {
        return R_FBI_INVALID_ARGUMENT;
    }

    Result res = 0;

    http_context ctx = (http_context) calloc(1, sizeof(struct http_context_s));
    if(ctx != NULL) {
        char currUrl[1024];
        string_copy(currUrl, url, sizeof(currUrl));

        char range[64];
        if(rangeEnd > rangeStart) {
            snprintf(range, sizeof(range), "%lu-%lu", rangeStart, rangeEnd);
        } else {
            snprintf(range, sizeof(range), "%lu-", rangeStart);
        }

        bool resolved = false;
        u32 redirectCount = 0;
        while(R_SUCCEEDED(res) && !resolved && redirectCount < 32) {
            if(R_SUCCEEDED(res = httpcOpenContext(&ctx->httpc, HTTPC_METHOD_GET, currUrl, 1))) {
                u32 response = 0;
                if(R_SUCCEEDED(res = httpcSetSSLOpt(&ctx->httpc, SSLCOPT_DisableVerify))
                   && (!userAgent || R_SUCCEEDED(res = httpcAddRequestHeaderField(&ctx->httpc, "User-Agent", HTTP_USER_AGENT)))
                   && (rangeStart == 0 || R_SUCCEEDED(res = httpcAddRequestHeaderField(&ctx->httpc, "Range", range)))
                   && R_SUCCEEDED(res = httpcAddRequestHeaderField(&ctx->httpc, "Accept-Encoding", "gzip, deflate"))
                   && R_SUCCEEDED(res = httpcSetKeepAlive(&ctx->httpc, HTTPC_KEEPALIVE_ENABLED))
                   && R_SUCCEEDED(res = httpcBeginRequest(&ctx->httpc))
                   && R_SUCCEEDED(res = httpcGetResponseStatusCodeTimeout(&ctx->httpc, &response, HTTP_TIMEOUT_NS))) {
                    if(response == 301 || response == 302 || response == 303) {
                        redirectCount++;

                        char redirectTo[1024];
                        memset(redirectTo, '\0', sizeof(redirectTo));
                        if(R_SUCCEEDED(res = httpcGetResponseHeader(&ctx->httpc, "Location", redirectTo, sizeof(redirectTo)))) {
                            httpcCloseContext(&ctx->httpc);

                            http_resolve_redirect(currUrl, redirectTo, sizeof(currUrl));
                        }
                    } else {
                        resolved = true;

                        if(response == 200) {
                            char encoding[32];
                            if(R_SUCCEEDED(httpcGetResponseHeader(&ctx->httpc, "Content-Encoding", encoding, sizeof(encoding)))) {
                                bool gzip = strncmp(encoding, "gzip", sizeof(encoding)) == 0;
                                bool deflate = strncmp(encoding, "deflate", sizeof(encoding)) == 0;

                                ctx->compressed = gzip || deflate;

                                if(ctx->compressed) {
                                    memset(&ctx->inflate, 0, sizeof(ctx->inflate));
                                    if(deflate) {
                                        inflateInit(&ctx->inflate);
                                    } else if(gzip) {
                                        inflateInit2(&ctx->inflate, MAX_WBITS | 16);
                                    }
                                }
                            }
                        } else {
                            res = R_FBI_HTTP_ERROR_BASE + response;
                        }
                    }
                }

                if(R_FAILED(res)) {
                    httpcCloseContext(&ctx->httpc);
                }
            }
        }

        if(R_SUCCEEDED(res) && redirectCount >= 32) {
            res = R_FBI_HTTP_TOO_MANY_REDIRECTS;
        }

        if(R_FAILED(res)) {
            free(ctx);
        }
    } else {
        res = R_FBI_OUT_OF_MEMORY;
    }

    if(R_SUCCEEDED(res)) {
        *context = ctx;
    }

    return res;
}

Result http_close(http_context context) {
    if(context == NULL) {
        return R_FBI_INVALID_ARGUMENT;
    }

    if(context->compressed) {
        inflateEnd(&context->inflate);
    }

    Result res = httpcCloseContext(&context->httpc);
    free(context);
    return res;
}

Result http_get_size(http_context context, u32* size) {
    if(context == NULL || size == NULL) {
        return R_FBI_INVALID_ARGUMENT;
    }

    return httpcGetDownloadSizeState(&context->httpc, NULL, size);
}

Result http_get_file_name(http_context context, char* out, u32 size) {
    if(context == NULL || out == NULL) {
        return R_FBI_INVALID_ARGUMENT;
    }

    Result res = 0;

    char* header = (char*) calloc(1, size + 64);
    if(header != NULL) {
        if(R_SUCCEEDED(res = httpcGetResponseHeader(&context->httpc, "Content-Disposition", header, size + 64))) {
            char* start = strstr(header, "filename=");
            if(start != NULL) {
                char format[32];
                snprintf(format, sizeof(format), "filename=\"%%%lu[^\"]\"", size);
                if(sscanf(start, format, out) != 1) {
                    res = R_FBI_BAD_DATA;
                }
            } else {
                res = R_FBI_BAD_DATA;
            }
        }

        free(header);
    } else {
        res = R_FBI_OUT_OF_MEMORY;
    }

    return res;
}

Result http_read(http_context context, u32* bytesRead, void* buffer, u32 size) {
    if(context == NULL || buffer == NULL) {
        return R_FBI_INVALID_ARGUMENT;
    }

    Result res = 0;

    u32 startPos = 0;
    if(R_SUCCEEDED(res = httpcGetDownloadSizeState(&context->httpc, &startPos, NULL))) {
        res = HTTPC_RESULTCODE_DOWNLOADPENDING;

        u32 outPos = 0;
        if(context->compressed) {
            u32 lastPos = context->bufferSize;
            while(res == HTTPC_RESULTCODE_DOWNLOADPENDING && outPos < size) {
                if((context->bufferSize > 0
                    || R_SUCCEEDED(res = httpcReceiveDataTimeout(&context->httpc, &context->buffer[context->bufferSize], sizeof(context->buffer) - context->bufferSize, HTTP_TIMEOUT_NS))
                    || res == HTTPC_RESULTCODE_DOWNLOADPENDING)) {
                    Result posRes = 0;
                    u32 currPos = 0;
                    if(R_SUCCEEDED(posRes = httpcGetDownloadSizeState(&context->httpc, &currPos, NULL))) {
                        context->bufferSize += currPos - lastPos;

                        context->inflate.next_in = context->buffer;
                        context->inflate.next_out = buffer + outPos;
                        context->inflate.avail_in = context->bufferSize;
                        context->inflate.avail_out = size - outPos;
                        inflate(&context->inflate, Z_SYNC_FLUSH);

                        memcpy(context->buffer, context->buffer + (context->bufferSize - context->inflate.avail_in), context->inflate.avail_in);
                        context->bufferSize = context->inflate.avail_in;

                        lastPos = currPos;
                        outPos = size - context->inflate.avail_out;
                    } else {
                        res = posRes;
                    }
                }
            }
        } else {
            while(res == HTTPC_RESULTCODE_DOWNLOADPENDING && outPos < size) {
                if(R_SUCCEEDED(res = httpcReceiveDataTimeout(&context->httpc, &((u8*) buffer)[outPos], size - outPos, HTTP_TIMEOUT_NS)) || res == HTTPC_RESULTCODE_DOWNLOADPENDING) {
                    Result posRes = 0;
                    u32 currPos = 0;
                    if(R_SUCCEEDED(posRes = httpcGetDownloadSizeState(&context->httpc, &currPos, NULL))) {
                        outPos = currPos - startPos;
                    } else {
                        res = posRes;
                    }
                }
            }
        }

        if(res == HTTPC_RESULTCODE_DOWNLOADPENDING) {
            res = 0;
        }

        if(R_SUCCEEDED(res) && bytesRead != NULL) {
            *bytesRead = outPos;
        }
    }

    return res;
}

#define R_HTTP_TLS_VERIFY_FAILED 0xD8A0A03C

#define HTTP_CONTENT_LENGTH_HEADER "Content-Length"

typedef struct {
    u32 bufferSize;
    u64* contentLength;
    void* userData;
    Result (*callback)(void* userData, void* buffer, size_t size);

    void* buf;
    u32 pos;

    Result res;
} http_curl_data;

static size_t http_curl_header_callback(char* buffer, size_t size, size_t nitems, void* userdata) {
    http_curl_data* curlData = (http_curl_data*) userdata;

    size_t bytes = size * nitems;
    size_t headerNameLen = strlen(HTTP_CONTENT_LENGTH_HEADER);

    if(bytes >= headerNameLen && strncmp(buffer, HTTP_CONTENT_LENGTH_HEADER, headerNameLen) == 0) {
        char* separator = strstr(buffer, ": ");
        if(separator != NULL) {
            char* valueStart = separator + 2;

            char value[32];
            memset(value, '\0', sizeof(value));
            strncpy(value, valueStart, bytes - (valueStart - buffer));

            if(curlData->contentLength != NULL) {
                *(curlData->contentLength) = (u64) atoll(value);
            }
        }
    }

    return size * nitems;
}

static size_t http_curl_write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    http_curl_data* curlData = (http_curl_data*) userdata;

    size_t available = size * nmemb;
    while(available > 0) {
        size_t remaining = curlData->bufferSize - curlData->pos;
        size_t copySize = available < remaining ? available : remaining;

        memcpy((u8*) curlData->buf + curlData->pos, ptr, copySize);
        curlData->pos += copySize;
        available -= copySize;

        if(curlData->pos == curlData->bufferSize) {
            curlData->res = curlData->callback(curlData->userData, curlData->buf, curlData->bufferSize);
            curlData->pos = 0;
        }
    }

    return R_SUCCEEDED(curlData->res) ? size * nmemb : 0;
}

Result http_download_callback(const char* url, u32 bufferSize, u64* contentLength, void* userData, Result (*callback)(void* userData, void* buffer, size_t size)) {
    Result res = 0;

    void* buf = malloc(bufferSize);
    if(buf != NULL) {
        http_context context = NULL;
        if(R_SUCCEEDED(res = http_open(&context, url, true))) {
            u32 dlSize = 0;
            if(R_SUCCEEDED(res = http_get_size(context, &dlSize))) {
                if(contentLength != NULL) {
                    *contentLength = dlSize;
                }

                u32 total = 0;
                u32 currSize = 0;
                while(total < dlSize
                      && R_SUCCEEDED(res = http_read(context, &currSize, buf, bufferSize))
                      && R_SUCCEEDED(res = callback(userData, buf, currSize))) {
                    total += currSize;
                }

                Result closeRes = http_close(context);
                if(R_SUCCEEDED(res)) {
                    res = closeRes;
                }
            }
        } else if(res == R_HTTP_TLS_VERIFY_FAILED) {
            res = 0;

            CURL* curl = curl_easy_init();
            if(curl != NULL) {
                http_curl_data curlData = {bufferSize, contentLength, userData, callback, buf, 0, 0};

                curl_easy_setopt(curl, CURLOPT_URL, url);
                curl_easy_setopt(curl, CURLOPT_USERAGENT, HTTP_USER_AGENT);
                curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, bufferSize);
                curl_easy_setopt(curl, CURLOPT_TIMEOUT, HTTP_TIMEOUT_SEC);
                curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
                curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0);
                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_curl_write_callback);
                curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*) &curlData);
                curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, http_curl_header_callback);
                curl_easy_setopt(curl, CURLOPT_HEADERDATA, (void*) &curlData);

                CURLcode ret = curl_easy_perform(curl);

                if(ret == CURLE_OK && curlData.pos != 0) {
                    curlData.res = curlData.callback(curlData.userData, curlData.buf, curlData.pos);
                    curlData.pos = 0;

                    if(R_FAILED(curlData.res)) {
                        ret = CURLE_WRITE_ERROR;
                    }
                }

                if(ret != CURLE_OK) {
                    if(ret == CURLE_WRITE_ERROR) {
                        res = curlData.res;
                    } else {
                        res = R_FBI_CURL_ERROR_BASE + ret;
                    }
                }

                curl_easy_cleanup(curl);
            } else {
                res = R_FBI_CURL_INIT_FAILED;
            }
        }

        free(buf);
    } else {
        res = R_FBI_OUT_OF_MEMORY;
    }

    return res;
}

typedef struct {
    void* buf;
    size_t size;

    size_t pos;
} http_buffer_data;

static Result http_download_buffer_callback(void* userData, void* buffer, size_t size) {
    http_buffer_data* data = (http_buffer_data*) userData;

    size_t remaining = data->size - data->pos;
    size_t copySize = size;
    if(copySize > remaining) {
        copySize = remaining;
    }

    if(copySize > 0) {
        memcpy((u8*) data->buf + data->pos, buffer, copySize);
        data->pos += copySize;
    }

    return 0;
}


Result http_download_buffer(const char* url, u32* downloadedSize, void* buf, size_t size) {
    http_buffer_data data = {buf, size, 0};
    Result res = http_download_callback(url, size, NULL, &data, http_download_buffer_callback);

    if(R_SUCCEEDED(res)) {
        *downloadedSize = data.pos;
    }

    return res;
}

Result http_download_json(const char* url, json_t** json, size_t maxSize) {
    if(url == NULL || json == NULL) {
        return R_FBI_INVALID_ARGUMENT;
    }

    Result res = 0;

    char* text = (char*) calloc(sizeof(char), maxSize);
    if(text != NULL) {
        u32 textSize = 0;
        if(R_SUCCEEDED(res = http_download_buffer(url, &textSize, text, maxSize))) {
            json_error_t error;
            json_t* parsed = json_loads(text, 0, &error);
            if(parsed != NULL) {
                *json = parsed;
            } else {
                res = R_FBI_PARSE_FAILED;
            }
        }

        free(text);
    } else {
        res = R_FBI_OUT_OF_MEMORY;
    }

    return res;
}

static Result FSUSER_AddSeed(u64 titleId, const void* seed) {
    u32 *cmdbuf = getThreadCommandBuffer();

    cmdbuf[0] = 0x087A0180;
    cmdbuf[1] = (u32) (titleId & 0xFFFFFFFF);
    cmdbuf[2] = (u32) (titleId >> 32);
    memcpy(&cmdbuf[3], seed, 16);

    Result ret = 0;
    if(R_FAILED(ret = svcSendSyncRequest(*fsGetSessionHandle()))) return ret;

    ret = cmdbuf[1];
    return ret;
}

Result http_download_seed(u64 titleId) {
    char pathBuf[64];
    snprintf(pathBuf, 64, "/fbi/seed/%016llX.dat", titleId);

    Result res = 0;

    FS_Path* fsPath = fs_make_path_utf8(pathBuf);
    if(fsPath != NULL) {
        u8 seed[16];

        Handle fileHandle = 0;
        if(R_SUCCEEDED(res = FSUSER_OpenFileDirectly(&fileHandle, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), *fsPath, FS_OPEN_READ, 0))) {
            u32 bytesRead = 0;
            res = FSFILE_Read(fileHandle, &bytesRead, 0, seed, sizeof(seed));

            FSFILE_Close(fileHandle);
        }

        fs_free_path_utf8(fsPath);

        if(R_FAILED(res)) {
            u8 region = CFG_REGION_USA;
            CFGU_SecureInfoGetRegion(&region);

            if(region <= CFG_REGION_TWN) {
                static const char* regionStrings[] = {"JP", "US", "GB", "GB", "HK", "KR", "TW"};

                char url[128];
                snprintf(url, 128, "https://kagiya-ctr.cdn.nintendo.net/title/0x%016llX/ext_key?country=%s", titleId, regionStrings[region]);

                u32 downloadedSize = 0;
                if(R_SUCCEEDED(res = http_download_buffer(url, &downloadedSize, seed, sizeof(seed))) && downloadedSize != sizeof(seed)) {
                    res = R_FBI_BAD_DATA;
                }
            } else {
                res = R_FBI_OUT_OF_RANGE;
            }
        }

        if(R_SUCCEEDED(res)) {
            res = FSUSER_AddSeed(titleId, seed);
        }
    } else {
        res = R_FBI_OUT_OF_MEMORY;
    }

    return res;
}