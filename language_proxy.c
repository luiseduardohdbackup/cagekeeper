#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>
#include <sys/types.h>
#include <signal.h>
#include "language.h"
#include "dict.h"
#include "seccomp.h"

typedef struct _proxy_internal {
    language_t*li;
    language_t*old;
    pid_t child_pid;
    int fd_w;
    int fd_r;
    int timeout;
    int max_memory;
    dict_t*callback_functions;
    bool in_call;
} proxy_internal_t;

#define DEFINE_CONSTANT 1
#define DEFINE_FUNCTION 2
#define COMPILE_SCRIPT 3
#define IS_FUNCTION 4
#define CALL_FUNCTION 5

#define RESP_CALLBACK 10
#define RESP_RETURN 11

#define MAX_ARRAY_SIZE 1024
#define MAX_STRING_SIZE 4096

static void write_byte(int fd, uint8_t b)
{
    write(fd, &b, 1);
}

static void write_string(int fd, const char*name)
{
    int l = strlen(name);
    write(fd, &l, sizeof(l));
    write(fd, name, l);
}

static char* read_string(int fd, struct timeval* timeout)
{
    int l = 0;
    if(!read_with_timeout(fd, &l, sizeof(l), timeout))
        return NULL;
    if(l<0 || l>=MAX_STRING_SIZE)
        return NULL;
    char* s = malloc(l+1);
    if(!s)
        return NULL;
    if(!read_with_timeout(fd, s, l, timeout))
        return NULL;
    s[l]=0;
    return s;
}

static const char* write_value(int fd, value_t*v)
{ 
    char b = v->type;
    write(fd, &b, 1);

    switch(v->type) {
        case TYPE_VOID:
            return;
        case TYPE_FLOAT32:
            write(fd, &v->f32, sizeof(v->f32));
            return;
        case TYPE_INT32:
            write(fd, &v->i32, sizeof(v->i32));
            return;
        case TYPE_BOOLEAN:
            write(fd, &v->b, sizeof(v->b));
            return;
        case TYPE_STRING:
            write_string(fd, v->str);
            return;
        case TYPE_ARRAY:
            write(fd, &v->length, sizeof(v->length));
            int i;
            for(i=0;i<v->length;i++) {
                write_value(fd, v->data[i]);
            }
            return;
    }
}

static value_t* _read_value(int fd, int*count, struct timeval* timeout)
{ 
    char b = 0;
    if(!read_with_timeout(fd, &b, 1, timeout)) {
        return NULL;
    }
    value_t dummy;

    switch(b) {
        case TYPE_VOID:
            return value_new_void();
        case TYPE_FLOAT32:
            if(!read_with_timeout(fd, &dummy.f32, sizeof(dummy.f32), timeout)) {
                return NULL;
            }
            return value_new_float32(dummy.f32);
        case TYPE_INT32:
            if(!read_with_timeout(fd, &dummy.i32, sizeof(dummy.i32), timeout)) {
                return NULL;
            }
            return value_new_int32(dummy.i32);
        case TYPE_BOOLEAN:
            if(!read_with_timeout(fd, &dummy.b, sizeof(dummy.b), timeout)) {
                return NULL;
            }
            return value_new_boolean(!!dummy.b);
        case TYPE_STRING: {
            char*s = read_string(fd, timeout);
            if(!s)
                return NULL;
            value_t* v = value_new_string(s);
            free(s);
            return v;
        }
        case TYPE_ARRAY: {
            if(!read_with_timeout(fd, &dummy.length, sizeof(dummy.length), timeout)) {
                return NULL;
            }

            /* protect against int overflows */
            if(dummy.length >= MAX_ARRAY_SIZE)
                return NULL;
            if(dummy.length >= INT_MAX - *count)
                return NULL;

            if(dummy.length + *count >= MAX_ARRAY_SIZE)
                return NULL;

            value_t*array = array_new();
            int i;
            for(i=0;i<dummy.length;i++) {
                value_t*entry = _read_value(fd, count, timeout);
                if(entry == NULL) {
                    value_destroy(array);
                    return NULL;
                }
                array_append(array, entry);
            }
            *count += dummy.length;
            return array;
        }
        default:
            return NULL;
    }
}

static value_t* read_value(int fd, struct timeval* timeout)
{
    int count = 0;
    return _read_value(fd, &count, timeout);
}

static void define_constant_proxy(language_t*li, const char*name, value_t*value)
{
    proxy_internal_t*proxy = (proxy_internal_t*)li->internal;

    dbg("[proxy] define_constant(%s)", name);
    write_byte(proxy->fd_w, DEFINE_CONSTANT);
    write_string(proxy->fd_w, name);
    write_value(proxy->fd_w, value);
}

static void define_function_proxy(language_t*li, const char*name, function_t*f)
{
    proxy_internal_t*proxy = (proxy_internal_t*)li->internal;

    dbg("[proxy] define_function(%s)", name);
    
    /* let the child know that we're accepting callbacks for this function name */
    write_byte(proxy->fd_w, DEFINE_FUNCTION);
    write_string(proxy->fd_w, name);

    if(dict_contains(proxy->callback_functions, name)) {
        language_error(li, "function %s already defined", name);
        return;
    }

    dict_put(proxy->callback_functions, name, f);
}

static bool process_callbacks(language_t*li, struct timeval* timeout)
{
    proxy_internal_t*proxy = (proxy_internal_t*)li->internal;

    char resp = 0;
    if(!read_with_timeout(proxy->fd_r, &resp, 1, timeout)) {
        return false;
    }
 
    while(1) {
        switch(resp) {
            case RESP_CALLBACK: {
                char*name = read_string(proxy->fd_r, timeout);
                if(!name)
                    return false;
                value_t*args = read_value(proxy->fd_r, timeout);
                if(!args) {
                    free(name);
                    return false;
                }
                value_t*function = dict_lookup(proxy->callback_functions, name);
                if(!function) {
                    value_destroy(args);
                    free(name);
                    return false;
                }
                value_t*ret = function->call(function, args);
                write_value(proxy->fd_w, ret);
                value_destroy(ret);
                value_destroy(args);
                free(name);
            }
            break;
            case RESP_RETURN:
            return true;
        }
    }
}

static bool compile_script_proxy(language_t*li, const char*script)
{
    proxy_internal_t*proxy = (proxy_internal_t*)li->internal;

    dbg("[proxy] compile_script()");
    write_byte(proxy->fd_w, COMPILE_SCRIPT);
    write_string(proxy->fd_w, script);

    struct timeval timeout;
    timeout.tv_sec = proxy->timeout;
    timeout.tv_usec = 0;

    if(proxy->in_call) {
        language_error(li, "You called (or compiled) the guest program, and the guest program called back. You can't invoke the guest again from your callback function.");
        return NULL;
    }
    proxy->in_call = true;
    process_callbacks(li, &timeout);
    proxy->in_call = false;

    bool ret = false;
    if(!read_with_timeout(proxy->fd_r, &ret, 1, &timeout))
        return false;
    return !!ret;
}

static bool is_function_proxy(language_t*li, const char*name)
{
    proxy_internal_t*proxy = (proxy_internal_t*)li->internal;

    dbg("[proxy] is_function(%s)", name);
    write_byte(proxy->fd_w, IS_FUNCTION);
    write_string(proxy->fd_w, name);

    struct timeval timeout;
    timeout.tv_sec = proxy->timeout;
    timeout.tv_usec = 0;

    bool ret = false;
    if(!read_with_timeout(proxy->fd_r, &ret, 1, &timeout))
        return false;
    return !!ret;
}

static value_t* call_function_proxy(language_t*li, const char*name, value_t*args)
{
    proxy_internal_t*proxy = (proxy_internal_t*)li->internal;

    dbg("[proxy] call_function(%s)", name);
    write_byte(proxy->fd_w, CALL_FUNCTION);
    write_string(proxy->fd_w, name);
    write_value(proxy->fd_w, args);

    struct timeval timeout;
    timeout.tv_sec = proxy->timeout;
    timeout.tv_usec = 0;

    if(proxy->in_call) {
        language_error(li, "You called the guest program, and the guest program called back. You can't invoke the guest again from your callback function.");
        return NULL;
    }
    proxy->in_call = true;
    process_callbacks(li, &timeout);
    proxy->in_call = false;

    return read_value(proxy->fd_r, &timeout);
}

typedef struct _proxy_function {
    language_t*li;
    char*name;
} proxy_function_t;

static void proxy_function_destroy(value_t*v)
{
    proxy_function_t*f = (proxy_function_t*)v->internal; 
    free(f->name);
    free(v->internal);
    free(v);
}

static value_t* proxy_function_call(value_t*v, value_t*args)
{
    proxy_function_t*f = (proxy_function_t*)v->internal; 
    language_t*li = f->li;
    proxy_internal_t*proxy = (proxy_internal_t*)li->internal;

    write_byte(proxy->fd_w, RESP_CALLBACK);
    write_string(proxy->fd_w, f->name);
    write_value(proxy->fd_w, args);
    return read_value(proxy->fd_r, NULL);
}

static void child_loop(language_t*li)
{
    proxy_internal_t*proxy = (proxy_internal_t*)li->internal;
    language_t*old = proxy->old;

    int r = proxy->fd_r;
    int w = proxy->fd_w;

    while(1) {
        char command;
        if(!read_with_retry(r, &command, 1))
            _exit(1);

        dbg("[sandbox] command=%d", command);
        switch(command) {
            case DEFINE_CONSTANT: {
                char*s = read_string(r, NULL);
                dbg("[sandbox] define constant(%s)", s);
                value_t*v = read_value(r, NULL);
                old->define_constant(old, s, v);
                value_destroy(v);
            }
            break;
            case DEFINE_FUNCTION: {
                char*name = read_string(r, NULL);
                dbg("[sandbox] define function(%s)", name);

                proxy_function_t*pf = calloc(sizeof(proxy_function_t), 1);
                pf->li = li;
                pf->name = strdup(name);

                value_t*value = calloc(sizeof(value_t), 1);
                value->type = TYPE_FUNCTION;
                value->internal = pf;
                value->destroy = proxy_function_destroy;
                value->call = proxy_function_call;

                free(name);
            }
            break;
            case COMPILE_SCRIPT: {
                char*script = read_string(r, NULL);
                dbg("[sandbox] compile script");
                bool ret = old->compile_script(old, script);
                write_byte(w, RESP_RETURN);
                write_byte(w, ret);
                free(script);
            }
            break;
            case IS_FUNCTION: {
                char*function_name = read_string(r, NULL);
                dbg("[sandbox] is_function(%s)", function_name);
                bool ret = old->is_function(old, function_name);
                write_byte(w, ret);
                free(function_name);
            }
            break;
            case CALL_FUNCTION: {
                char*function_name = read_string(r, NULL);
                dbg("[sandbox] call_function(%s)", function_name, old->name);
                value_t*args = read_value(r, NULL);
                value_t*ret = old->call_function(old, function_name, args);
                dbg("[sandbox] returning function value (%s)", type_to_string(ret->type));
                write_byte(w, RESP_RETURN);
                write_value(w, ret);
                free(function_name);
                value_destroy(args);
                value_destroy(ret);
            }
            break;
            default: {
                fprintf(stderr, "Invalid command %d\n", command);
            }
        }
    }
}

static bool spawn_child(language_t*li)
{
    proxy_internal_t*proxy = (proxy_internal_t*)li->internal;

    int p_to_c[2];
    int c_to_p[2];
    int stdout_pipe[2];
    int stderr_pipe[2];

    if(pipe(p_to_c) || pipe(c_to_p) || pipe(stdout_pipe) || pipe(stderr_pipe)) {
        perror("create pipe");
        return false;
    }

    proxy->child_pid = fork();
    if(!proxy->child_pid) {
        //child
        close(p_to_c[1]); // close write
        close(c_to_p[0]); // close read
        proxy->fd_r = p_to_c[0];
        proxy->fd_w = c_to_p[1];

        // TODO
        //close(1); // close stdout
        //close(2); // close stderr

        printf("[child] entering secure environment\n");
        seccomp_lockdown(proxy->max_memory);
        printf("[child] running in seccomp mode\n");

        child_loop(li);
        _exit(0);
    }

    //parent
    close(c_to_p[1]); // close write
    close(p_to_c[0]); // close read
    proxy->fd_r = c_to_p[0];
    proxy->fd_w = p_to_c[1];
    return true;
}

static void destroy_proxy(language_t* li)
{
    proxy_internal_t*proxy = (proxy_internal_t*)li->internal;

    language_t*old = proxy->old;

    kill(proxy->child_pid, SIGKILL);
    int status = 0;
    int ret = waitpid(proxy->child_pid, &status, WNOHANG);
    if(WIFSIGNALED(status)) {
        printf("%08x signal=%d\n", ret, WTERMSIG(status));
    } else if(WIFEXITED(status)) {
        printf("%08x exit=%d\n", ret, WEXITSTATUS(status));
    } else {
        printf("%08x unknown exit reason. status=%d\n", ret, status);
    }
    free(proxy);
    free(li);

    old->destroy(old);
}

language_t* proxy_new(language_t*old, int max_memory)
{
    language_t * li = calloc(1, sizeof(language_t));
#ifdef DEBUG
    li->magic = LANG_MAGIC;
#endif
    li->name = "proxy";
    li->compile_script = compile_script_proxy;
    li->is_function = is_function_proxy;
    li->call_function = call_function_proxy;
    li->define_function = define_function_proxy;
    li->define_constant = define_constant_proxy;
    li->destroy = destroy_proxy;
    li->internal = calloc(1, sizeof(proxy_internal_t));

    proxy_internal_t*proxy = (proxy_internal_t*)li->internal;
    proxy->li = li;
    proxy->old = old;
    proxy->timeout = 10;
    proxy->max_memory = max_memory;

    if(!spawn_child(li)) {
        fprintf(stderr, "Couldn't spawn child process\n");
        free(proxy);
        free(li);
        return NULL;
    }

    proxy->callback_functions = dict_new(&ptr_type);

    return li;
}