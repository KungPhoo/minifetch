# Portability, correctness, and concurrency review

The changes use Windows' intended behavior as the native reference: one request,
a returned status/body/cookies, and redirects returned to the caller. They keep
`client.request` and `client.fetch()` for existing single-threaded callers.

## Significant findings addressed

| Severity | Original problem | Resulting behavior |
| --- | --- | --- |
| High | Emscripten copied an arbitrary method with `strcpy` into `requestMethod[32]`. | Method length is checked before copying; setup failure is reported. |
| High | Public request mutation, cookie reads/writes, and libcurl's implicit global initialization were unsynchronized. | `fetch(Request)` owns each operation's input, cookie state is mutex-protected, and libcurl initialization uses a C++ synchronized local static. |
| High | POSIX computed a POST body but never supplied it to libcurl. Raw data was also omitted. | Form, raw/binary, multipart, and PUT/PATCH bodies are sent with explicit sizes. |
| High | Emscripten had no POST/header implementation, dereferenced a potentially null fetch result, kept only status-200 bodies, and synchronously fetched on the main browser thread. | The public fetch API wraps asynchronous Fetch, owns buffer lifetimes, preserves HTTP error bodies, and yields through Asyncify until completion. |
| High | Cookies were retained forever, ignored Path/Secure/expiry, crossed HTTP/HTTPS on one host, and were percent-encoded again on replay. | Native cookies are isolated by exact origin, filtered by path/Secure/expiry, support deletion, and preserve accepted cookie octets. |
| Medium | Windows called `WinHttpSendRequest` only if the body was nonempty, making ordinary GET and empty POST fail. | Requests are sent even with no body; bodies also work for methods other than POST. |
| Medium | Windows' header helper ignored its argument and queried Set-Cookie even for Location. | The helper accepts the actual WinHTTP header selector and retrieves Location for 3xx responses. |
| Medium | Explicit ports, existing query strings, query-only URLs, fragments, and IPv6 authorities were mishandled. | Parsing separates host/port, preserves query strings, removes fragments, and rejects unsupported schemes/userinfo/invalid input. |
| Medium | Multipart text fields were URL-encoded rather than sent as their actual content; unreadable files silently became empty uploads. | Multipart text/binary data is preserved; unreadable files cause a setup error. |
| Medium | C callbacks could throw C++ allocation exceptions across the C boundary; error paths leaked transport resources. | libcurl callbacks catch failures, resources have RAII guards, and transport/setup failures have a separate error field. Browser callbacks remain private to the implementation. |
| Medium | Unchecked header strings allowed CR/LF injection; user-supplied framing could contradict the actual body size. | Invalid names/values are rejected; Content-Length and Transfer-Encoding are owned by the backend. |
| Medium | CMake set C++20 after creating the library, always built its demo regardless of the option, and browser flags were attached only to the demo. | C++17 is a public target requirement, the demo is optional, and required browser flags propagate to consumers. |
| Low | Standard headers were missing and the curl macro split a lambda at a comma, breaking Linux compilation. | Headers are explicit and callbacks are named before passing them to curl. No external compatibility header is needed. |
| Low | IM Used was numbered 229 and the status enum's inferred range did not cover all HTTP status codes. | IM Used is 226; a fixed `int` underlying type supports nonstandard codes such as 599. |

## Thread-safety contract and cost

Use `client.fetch(request)` concurrently, with a separate Request per caller.
The library copies/moves that input into an operation; it does not mutate the
caller's Request. A Request being copied must not be concurrently modified by
another thread. `cookieJar()` and `clearCookies()` are synchronized too.

There is one mutex and cookie-state allocation per client. Operations only lock
while copying applicable cookies and merging received cookies; the mutex is never
held during networking. Each operation has its own WinHTTP/libcurl handles. This
avoids a thread pool, a global network lock, and a lock per body chunk. libcurl's
process-wide setup runs once. No throughput benchmark is claimed.

The legacy `client.request` field cannot be made safe for arbitrary concurrent
writes with an internal mutex: those writes bypass library methods. Externally
synchronize edits and legacy `fetch()`, or use the Request-taking overload.
Do not destroy a client during active native member calls. Join native caller
threads before process shutdown. Concurrent cookie updates resolve in completion
order; a completed in-flight request can add cookies after `clearCookies()`.
CookieState is forward-declared in the public header; mutex, chrono, optional,
and utility headers are implementation details in minifetch.cpp.
Clients are now noncopyable; create a separate client for a separate session.

## Browser differences and remaining limitations

- Emscripten uses XMLHttpRequest through its Fetch API. Browsers automatically
  follow redirects; native manual-redirect semantics cannot be faithfully exposed
  by this backend. Location may be absent and the returned status may be final.
- Browser-managed cookies are separate from the native jar. Set-Cookie is hidden,
  explicit Cookie/Host/User-Agent headers are not set, and `request.cookies`
  cannot override the browser store. `withCredentials` defaults to true and can
  be disabled. `clearCookies()` does not clear browser cookies.
- CORS must permit the origin, credentials, and custom headers. A browser network,
  timeout, or CORS failure has no HTTP status. Applications must serve WebAssembly
  over HTTP(S). `fetch()` starts asynchronous Fetch internally and yields with
  `emscripten_sleep` until the success/error callback completes. Linking requires
  Fetch and Asyncify; the CMake target propagates both. Calls must originate from
  an Asyncify-capable entry point. Do not initiate overlapping suspended calls on
  the same browser thread: Asyncify cannot suspend multiple reentrant stacks.
- The native cookie jar is deliberately conservative, not an RFC-complete browser
  store. Domain attributes do not enable subdomain sharing; origins/ports stay
  isolated. RFC1123 Expires, Max-Age, Path, and Secure are supported. Other legacy
  date formats, SameSite navigation rules, and browser cookie-prefix rules are
  not implemented. Multiple cookies with one name are reduced to the most
  specific matching path. `cookieJar()` is a diagnostic name/value snapshot keyed
  by origin, not a complete serialization of cookie metadata.
- Bodies/uploads are buffered in memory; streaming, cancellation, connection
  pooling, retries, and response-size limits remain future work. Request timeout
  semantics differ: curl/browser apply a transfer timeout, while WinHTTP exposes
  phase timeouts. Out-of-memory failures may still throw normal C++ exceptions.

## Suggested API direction

The implemented `Response fetch(Request)` is the smallest useful change: client
session state is separate from request state, which also enables concurrency.
The sample now uses it. Preserve the old spelling for migration, then consider
removing the public mutable `request` in a major release.

For a future major version, prefer a checked URL factory, explicit HTTP method,
`std::chrono` timeouts, and one explicit body kind (raw bytes, form, or multipart).
That avoids surprising GET-to-POST inference and appending raw bytes to a form.
Keep transport failure separate from HTTP status; an HTTP 404 is a valid response.
Use the same async completion model on all platforms if portable async usage
becomes a goal. C++17 callbacks are sufficient; coroutines need not be required.

## Validation

- Native fixture tests exercise GET/HEAD/empty POST, form/binary POST, PUT,
  multipart uploads, ports/query parsing, headers, redirects, HTTP error bodies,
  cookie scoping/expiry/deletion, invalid input, and transport failures.
- Eight requests on one client must reach a fixture barrier concurrently;
  a simultaneous cookie snapshot reader runs while each response writes a unique
  cookie. A mutex around the whole fetch would fail this test.
- The browser contract test compiles the Emscripten branch against a small test
  double based on the 4.0.19 Fetch interface. It checks bounded method copying,
  retained body/header lifetimes while fetch waits for a deferred callback,
  error callbacks, HTTP error bodies, timeout saturation, handle cleanup, and
  compatibility of both fetch overloads.
  It is not a WebAssembly build or a real browser integration test.
- GCC 14 C++17 AddressSanitizer/UndefinedBehaviorSanitizer and C++20
  ThreadSanitizer runs passed both suites. Windows source was cross-compiled
  with Zig 0.14.1 targeting x86_64-windows-gnu; no Windows execution was available.
- A full Emscripten SDK install was attempted, but its download host
  `storage.googleapis.com` was denied by the current network policy. Real
  Emscripten/browser validation remains outstanding. Network settings and the
  published environment configuration were not changed.
