/* ========================================================================
   http.serve — a small, real HTTP/1.1 server.

   The handler receives a request Map and returns either Text (sent as the
   body with status 200) or a Map with any of: status, body, type, headers.
   ======================================================================== */

static volatile bool g_http_running = false;

static void http_put(VM *vm, ObjMap *m, const char *k, const char *v) {
    vm_map_set(vm, m, v_obj((Obj *)vm_text_cstr(vm, k)), v_obj((Obj *)vm_text_cstr(vm, v)));
}

static const char *http_status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        default: return "OK";
    }
}

static Value http_serve_native(VM *vm, Value *args, int nargs, bool *ok) {
    int port = (int)v_toint(nargs > 0 ? args[0] : v_int(8080));
    Value handler = nargs > 1 ? args[1] : v_nil();

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { vm_runtime_error(vm, "could not open a socket: %s", strerror(errno)); *ok = false; return v_nil(); }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        close(fd);
        vm_runtime_error(vm, "port %d is already in use", port);
        *ok = false;
        return v_nil();
    }
    listen(fd, 128);
    g_http_running = true;
    fprintf(stderr, "%sserving%s http://localhost:%d  %s(ctrl-c to stop)%s\n", C_AMBER, C_RESET, port, C_DIM, C_RESET);

    while (g_http_running && !vm->had_error) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        struct timeval tv = { 1, 0 };
        sprfst_unlock();
        int ready = select(fd + 1, &set, NULL, NULL, &tv);
        sprfst_lock();
        if (ready <= 0) continue;
        int c = accept(fd, NULL, NULL);
        if (c < 0) continue;

        StrBuf raw; sb_init(&raw);
        char buf[4096];
        ssize_t n;
        size_t header_end = 0;
        long content_length = 0;
        for (;;) {
            sprfst_unlock();
            n = recv(c, buf, sizeof buf, 0);
            sprfst_lock();
            if (n <= 0) break;
            sb_put(&raw, buf, (size_t)n);
            char *hdr = raw.data ? strstr(raw.data, "\r\n\r\n") : NULL;
            if (hdr) {
                header_end = (size_t)(hdr - raw.data) + 4;
                const char *cl = strcasestr(raw.data, "content-length:");
                content_length = cl ? strtol(cl + 15, NULL, 10) : 0;
                if ((long)(raw.len - header_end) >= content_length) break;
            }
            if (raw.len > (1u << 22)) break;
        }
        if (!raw.data || !header_end) { close(c); sb_free(&raw); continue; }

        /* ----- parse */
        ObjMap *req = vm_map(vm);
        ObjMap *headers = vm_map(vm);
        char method[16] = "GET", target[2048] = "/";
        sscanf(raw.data, "%15s %2047s", method, target);
        char *qmark = strchr(target, '?');
        char query[1024] = "";
        if (qmark) { snprintf(query, sizeof query, "%s", qmark + 1); *qmark = 0; }
        http_put(vm, req, "method", method);
        http_put(vm, req, "path", target);
        http_put(vm, req, "query", query);

        char *line = strstr(raw.data, "\r\n");
        if (line) {
            line += 2;
            while (line < raw.data + header_end - 2) {
                char *eol = strstr(line, "\r\n");
                if (!eol || eol == line) break;
                *eol = 0;
                char *colon = strchr(line, ':');
                if (colon) {
                    *colon = 0;
                    char *val = colon + 1;
                    while (*val == ' ') val++;
                    for (char *q = line; *q; q++) *q = (char)tolower((unsigned char)*q);
                    http_put(vm, headers, line, val);
                }
                line = eol + 2;
            }
        }
        vm_map_set(vm, req, v_obj((Obj *)vm_text_cstr(vm, "headers")), v_obj((Obj *)headers));
        vm_map_set(vm, req, v_obj((Obj *)vm_text_cstr(vm, "body")),
                   v_obj((Obj *)vm_text(vm, raw.data + header_end, (int)(raw.len - header_end))));

        /* ----- dispatch */
        Value rq = v_obj((Obj *)req);
        Value res = handler.tag == V_OBJ ? vm_call_value(vm, handler, &rq, 1) : v_nil();

        int status = 200;
        const char *ctype = "text/html; charset=utf-8";
        const char *body = "";
        char *owned = NULL;
        if (IS_OBJ(res, O_MAP)) {
            ObjMap *m = AS_MAP(res);
            bool found = false;
            Value s = vm_map_get(m, v_obj((Obj *)vm_text_cstr(vm, "status")), &found);
            if (found) status = (int)v_toint(s);
            Value t = vm_map_get(m, v_obj((Obj *)vm_text_cstr(vm, "type")), &found);
            if (found && IS_OBJ(t, O_TEXT)) ctype = AS_TEXT(t)->chars;
            Value b = vm_map_get(m, v_obj((Obj *)vm_text_cstr(vm, "body")), &found);
            if (found) { owned = vm_value_text(vm, b, false); body = owned; }
        } else if (IS_OBJ(res, O_TEXT)) {
            body = AS_TEXT(res)->chars;
        } else if (res.tag != V_NIL) {
            owned = vm_value_text(vm, res, false);
            body = owned;
        } else {
            status = 404;
            body = "not found";
            ctype = "text/plain; charset=utf-8";
        }

        StrBuf out; sb_init(&out);
        sb_printf(&out, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                        "Server: SPRFST/%s\r\nConnection: close\r\n\r\n",
                  status, http_status_text(status), ctype, strlen(body), SPRFST_VERSION);
        sb_puts(&out, body);
        sprfst_unlock();
        ssize_t sent = 0;
        while (sent < (ssize_t)out.len) {
            ssize_t w = send(c, out.data + sent, out.len - (size_t)sent, 0);
            if (w <= 0) break;
            sent += w;
        }
        close(c);
        sprfst_lock();
        sb_free(&out);
        sb_free(&raw);
        free(owned);
    }
    close(fd);
    g_http_running = false;
    return v_nil();
}
