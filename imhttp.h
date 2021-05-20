#ifndef IMHTTP_H_
#define IMHTTP_H_

#include <assert.h>
// TODO: imhttp depends on a separate translation unit which makes it no longer header-only
#include "./sv.h"

typedef void* ImHTTP_Socket;
typedef ssize_t (*ImHTTP_Write)(ImHTTP_Socket socket, const void *buf, size_t count);
typedef ssize_t (*ImHTTP_Read)(ImHTTP_Socket socket, void *buf, size_t count);

// TODO: not all methods are supported
typedef enum {
    IMHTTP_GET,
    IMHTTP_POST,
} ImHTTP_Method;

#define IMHTTP_ROLLIN_BUFFER_CAPACITY (8 * 1024)
#define IMHTTP_USER_BUFFER_CAPACITY IMHTTP_ROLLIN_BUFFER_CAPACITY

static_assert(IMHTTP_ROLLIN_BUFFER_CAPACITY <= IMHTTP_USER_BUFFER_CAPACITY,
              "The user buffer should be at least as big as the rolling buffer "
              "because sometimes you may wanna put the whole rollin content into "
              "the user buffer.");

typedef struct {
    ImHTTP_Socket socket;
    ImHTTP_Write write;
    ImHTTP_Read read;

    char rollin_buffer[IMHTTP_ROLLIN_BUFFER_CAPACITY];
    size_t rollin_buffer_size;

    char user_buffer[IMHTTP_USER_BUFFER_CAPACITY];

    int content_length;
} ImHTTP;

void imhttp_req_begin(ImHTTP *imhttp, ImHTTP_Method method, const char *resource);
void imhttp_req_header(ImHTTP *imhttp, const char *header_name, const char *header_value);
void imhttp_req_headers_end(ImHTTP *imhttp);
void imhttp_req_body_chunk(ImHTTP *imhttp, const char *chunk_cstr);
void imhttp_req_body_chunk_sized(ImHTTP *imhttp, const char *chunk, size_t chunk_size);
void imhttp_req_end(ImHTTP *imhttp);

void imhttp_res_begin(ImHTTP *imhttp);
uint64_t imhttp_res_status_code(ImHTTP *imhttp);
bool imhttp_res_next_header(ImHTTP *imhttp, String_View *name, String_View *value);
bool imhttp_res_next_body_chunk(ImHTTP *imhttp, String_View *chunk);
void imhttp_res_end(ImHTTP *imhttp);

#endif // IMHTTP_H_

#ifdef IMHTTP_IMPLEMENTATION

static String_View imhttp_shift_rollin_buffer(ImHTTP *imhttp, const char *end)
{
    // Boundary check
    assert(imhttp->rollin_buffer <= end);
    size_t n = end - imhttp->rollin_buffer;
    assert(n <= imhttp->rollin_buffer_size);

    // Copy chunk to user buffer
    assert(n <= IMHTTP_USER_BUFFER_CAPACITY);
    memcpy(imhttp->user_buffer, imhttp->rollin_buffer, n);

    // Shifting the rollin buffer
    imhttp->rollin_buffer_size -= n;
    memmove(imhttp->rollin_buffer, end, imhttp->rollin_buffer_size);

    return (String_View) {
        .data = imhttp->user_buffer,
        .count = n,
    };
}

// TODO: document that imhttp_top_rollin_buffer does not perform any reads until *everything* inside of rollin_buffer is processed
static void imhttp_top_rollin_buffer(ImHTTP *imhttp)
{
    if (imhttp->rollin_buffer_size == 0) {
        ssize_t n = imhttp->read(
                        imhttp->socket,
                        imhttp->rollin_buffer + imhttp->rollin_buffer_size,
                        IMHTTP_ROLLIN_BUFFER_CAPACITY - imhttp->rollin_buffer_size);
        // TODO: imhttp_top_rollin_buffer() does not handle read errors
        assert(n > 0);
        imhttp->rollin_buffer_size += n;
    }
}

static String_View imhttp_rollin_buffer_as_sv(ImHTTP *imhttp)
{
    return (String_View) {
        .data = imhttp->rollin_buffer,
        .count = imhttp->rollin_buffer_size,
    };
}

static const char *imhttp_method_as_cstr(ImHTTP_Method method)
{
    switch (method) {
    case IMHTTP_GET:
        return "GET";
    case IMHTTP_POST:
        return "POST";
    default:
        assert(0 && "imhttp_method_as_cstr: unreachable");
    }
}

static void imhttp_write_cstr(ImHTTP *imhttp, const char *cstr)
{
    size_t cstr_size = strlen(cstr);
    // TODO: imhttp_write_cstr does not handle ImHTTP_Write errors
    imhttp->write(imhttp->socket, cstr, cstr_size);
}

void imhttp_req_begin(ImHTTP *imhttp, ImHTTP_Method method, const char *resource)
{
    imhttp_write_cstr(imhttp, imhttp_method_as_cstr(method));
    imhttp_write_cstr(imhttp, " ");
    // TODO: it is easy to make the resource malformed in imhttp_req_begin
    imhttp_write_cstr(imhttp, resource);
    imhttp_write_cstr(imhttp, " HTTP/1.1\r\n");
}

void imhttp_req_header(ImHTTP *imhttp, const char *header_name, const char *header_value)
{
    imhttp_write_cstr(imhttp, header_name);
    imhttp_write_cstr(imhttp, ": ");
    imhttp_write_cstr(imhttp, header_value);
    imhttp_write_cstr(imhttp, "\r\n");
}

void imhttp_req_headers_end(ImHTTP *imhttp)
{
    imhttp_write_cstr(imhttp, "\r\n");
}

void imhttp_req_body_chunk(ImHTTP *imhttp, const char *chunk_cstr)
{
    imhttp_write_cstr(imhttp, chunk_cstr);
}

void imhttp_req_body_chunk_sized(ImHTTP *imhttp, const char *chunk, size_t chunk_size)
{
    imhttp->write(imhttp->socket, chunk, chunk_size);
}

void imhttp_req_end(ImHTTP *imhttp)
{
    (void) imhttp;
}

void imhttp_res_begin(ImHTTP *imhttp)
{
    // Reset the Content-Length
    imhttp->content_length = -1;
}

uint64_t imhttp_res_status_code(ImHTTP *imhttp)
{
    imhttp_top_rollin_buffer(imhttp);
    String_View rollin = imhttp_rollin_buffer_as_sv(imhttp);
    String_View status_line = sv_chop_by_delim(&rollin, '\n');
    assert(
        sv_ends_with(status_line, SV("\r")) &&
        "The rolling buffer is so small that it could not fit the whole status line. "
        "Or maybe the status line was not fully read after the imhttp_top_rollin_buffer() "
        "above");
    status_line = imhttp_shift_rollin_buffer(imhttp, rollin.data);
    // TODO: the HTTP version is skipped in imhttp_res_status_code()
    sv_chop_by_delim(&status_line, ' ');
    String_View code_sv = sv_chop_by_delim(&status_line, ' ');
    return sv_to_u64(code_sv);
}

// TODO: Document that imhttp_res_next_header() invalidate name and value on the consequent imhttp_* calls
bool imhttp_res_next_header(ImHTTP *imhttp, String_View *name, String_View *value)
{
    imhttp_top_rollin_buffer(imhttp);
    String_View rollin = imhttp_rollin_buffer_as_sv(imhttp);
    String_View header_line = sv_chop_by_delim(&rollin, '\n');
    assert(
        sv_ends_with(header_line, SV("\r")) &&
        "The rolling buffer is so small that it could not fit the whole header line. "
        "Or maybe the header line was not fully read after the imhttp_top_rollin_buffer() "
        "above");
    // Transfer the ownership of header_line from rollin_buffer to user_buffer
    header_line = imhttp_shift_rollin_buffer(imhttp, rollin.data);

    if (!sv_eq(header_line, SV("\r\n"))) {
        // TODO: don't set name/value if the user set them to NULL in imhttp_res_next_header
        *name = sv_chop_by_delim(&header_line, ':');
        *value = sv_trim(header_line);

        // TODO: are header case-sensitive?
        if (sv_eq(*name, SV("Content-Length"))) {
            // TODO: content_length overflow
            imhttp->content_length = sv_to_u64(*value);
        }
        return true;
    }

    return false;
}

// TODO: document that the chunk is always invalidated after each imhttp_res_next_body_chunk() call
bool imhttp_res_next_body_chunk(ImHTTP *imhttp, String_View *chunk)
{
    // TODO: ImHTTP can't handle the responses that do not set Content-Length
    assert(imhttp->content_length >= 0);

    if (imhttp->content_length > 0) {
        imhttp_top_rollin_buffer(imhttp);
        String_View rollin = imhttp_rollin_buffer_as_sv(imhttp);
        // TODO: ImHTTP does not handle the situation when the server responded with more data than it claimed with Content-Length
        assert(rollin.count <= (size_t) imhttp->content_length);

        String_View result = imhttp_shift_rollin_buffer(
                                 imhttp,
                                 imhttp->rollin_buffer + imhttp->rollin_buffer_size);

        if (chunk) {
            *chunk = result;
        }

        imhttp->content_length -= result.count;

        return true;
    }

    return false;
}

void imhttp_res_end(ImHTTP *imhttp)
{
    (void) imhttp;
}

#endif // IMHTTP_IMPLEMENTATION
