#pragma once
// HTTPS GET for the mod browser (Windows: WinHTTP; Linux: the system's libcurl, loaded when
// first needed). runtime/net/http.h builds its downloads on it.
#include "../net/http.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

// GET url (redirects followed, never from https to http), passing the body to sink in
// pieces; false with a message on failure, a non-200 status, a body over maxBytes (by its
// Content-Length or while reading), a sink that returned false or progress->cancel.
bool Http_Fetch(const std::string& url, const std::function<bool(const uint8_t*, size_t)>& sink, std::string& error,
                HttpProgress* progress, uint64_t maxBytes);
