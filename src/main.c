// NOTE(nic): we need to define this in order to have `popen` function
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <assert.h>

#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "./json.h"
#include "./utils.h"

#define ARENA_IMPLEMENTATION
#include "./arena.h"

#define I3_MAGIC "i3-ipc"
#define I3_HEADER_SIZE 14 // in bytes

void str_append_uint32_bytes_le(Arena *arena, String *str, uint32_t n) {
    for (size_t i = 0; i < sizeof(uint32_t); ++i) {
        char ch = (n >> (i * 8)) & 0xFF;
        str_append_char(arena, str, ch);
    }
}

typedef struct {
    const uint8_t *data;
    size_t size;
} Bytes_View;

typedef struct {
    const uint8_t *bytes;
    size_t size;
    size_t cursor;
} Bytes_Reader;

Bytes_Reader reader_from_str(String *str) {
    Bytes_Reader reader = {0};
    reader.bytes = (const uint8_t *)str->items;
    reader.size = str->count;
    return reader;
}

Bytes_View reader_read_bytes(Bytes_Reader *reader, size_t bytes_count) {
    assert(reader->cursor + bytes_count <= reader->size && "Oops, reading past the end of bytes view");
    Bytes_View view = {0};
    view.data = &reader->bytes[reader->cursor];
    view.size = bytes_count;
    reader->cursor += bytes_count;
    return view;
}

uint32_t reader_read_uint32_bytes_le(Bytes_Reader *reader) {
    Bytes_View uint32_bytes = reader_read_bytes(reader, sizeof(uint32_t));
    uint32_t n = 0;
    for (size_t i = 0; i < sizeof(uint32_t); ++i) {
        n |= ((uint32_bytes.data[i] << (i * 8)));
    }
    return n;
}

// NOTE(nic): it's kind of sad that we need an arena to find the scrathcpad
// memory allocated here is just a few bytes (even considering it's recursive)
Json_Dict *i3_find_scratchpad(Arena *arena, Json_Array *nodes) {
    for (size_t i = 0; i < nodes->count; ++i) {
        Json_Dict *dict = json_array_get_dict(nodes, i);
        Json_Array *subnodes = json_dict_get_array(dict, json_obj_string(arena, "nodes"));
        if (subnodes != NULL) {
            Json_Dict *scratchpad = i3_find_scratchpad(arena, subnodes);
            if (scratchpad != NULL) {
                return scratchpad;
            }
        }

        String *node_type = json_dict_get_string(dict, json_obj_string(arena, "type"));
        String *node_name = json_dict_get_string(dict, json_obj_string(arena, "name"));
        if (str_eq_cstr(node_type, "workspace") && str_eq_cstr(node_name, "__i3_scratch")) {
            return dict;
        }
    }
    return NULL;
}

typedef struct {
    int64_t id;
    String_View name;
} Window;

typedef struct {
    Window *items;
    size_t count;
    size_t capacity;
} Windows;

void i3_get_node_windows_impl(Arena *arena, Windows *windows, Json_Dict *curr, Json_Dict *parent) {
    Json_Array *nodes = json_dict_get_array(curr, json_obj_string(arena, "nodes"));
    Json_Array *floating_nodes = json_dict_get_array(curr, json_obj_string(arena, "floating_nodes"));

    if (parent != NULL) {
        String *node_type = json_dict_get_string(curr, json_obj_string(arena, "type"));
        String *parent_type = json_dict_get_string(parent, json_obj_string(arena, "type"));
        if (nodes->count <= 0
            && floating_nodes->count <= 0
            && str_eq_cstr(node_type, "con")
            && !str_eq_cstr(parent_type, "dockarea"))
        {
            // NOTE(nic): yes, we use the class as the window name, don't ask questions
            Json_Dict *window_props = json_dict_get_dict(curr, json_obj_string(arena, "window_properties"));
            int64_t *window_id = json_dict_get_int64(curr, json_obj_string(arena, "id"));

            String *window_name = json_dict_get_string(window_props, json_obj_string(arena, "class"));
            if (window_name == NULL) {
                window_name = json_dict_get_string(window_props, json_obj_string(arena, "title"));
            }
            assert(window_name != NULL);

            size_t window_index = windows->count + 1;
            String str = {0};
            str_append_fmt(arena, &str, "%d. ", window_index);
            arena_da_append_many(arena, &str, window_name->items, window_name->count);

            String_View actual_window_name = (String_View) { str.items, str.count };
            Window window = { *window_id, actual_window_name };
            arena_da_append(arena, windows, window);
        }
    }

    for (size_t i = 0; i < nodes->count; ++i) {
        Json_Dict *subnode = json_array_get_dict(nodes, i);
        i3_get_node_windows_impl(arena, windows, subnode, curr);
    }
    for (size_t i = 0; i < floating_nodes->count; ++i) {
        Json_Dict *subnode = json_array_get_dict(floating_nodes, i);
        i3_get_node_windows_impl(arena, windows, subnode, curr);
    }
}

Windows i3_get_scratchpad_windows(Arena *arena, Json_Dict *node) {
    Windows windows = {0};
    i3_get_node_windows_impl(arena, &windows, node, NULL);
    return windows;
}

Json_Result i3_receive_message(Arena *arena, int socket_fd, Json_Object *object) {
    uint8_t header[I3_HEADER_SIZE];
    ssize_t header_bytes_received = recv(socket_fd, header, I3_HEADER_SIZE, MSG_WAITALL);
    if (header_bytes_received != I3_HEADER_SIZE) {
        fprintf(stderr, "Error: could not receive message: %s\n", strerror(errno));
        exit(1);
    }

    Bytes_Reader reader = { header, I3_HEADER_SIZE, 0 };
    (void)reader_read_bytes(&reader, strlen(I3_MAGIC));
    uint32_t message_size = reader_read_uint32_bytes_le(&reader);
    (void)reader_read_uint32_bytes_le(&reader);

    String message = str_with_cap(arena, message_size);
    message.count = message_size;

    ssize_t message_bytes_received = recv(socket_fd, message.items, message_size, 0);
    if (message_bytes_received != message_size) {
        fprintf(stderr, "Error: could not receive message: %s\n", strerror(errno));
        exit(1);
    }

    return json_parse(arena, object, message.items, message.count);
}

typedef struct {
    int failed;
    const char *error;
    ssize_t index;
} Prompt_Result;

Prompt_Result prompt_user(Arena *arena, Windows *windows) {
    Prompt_Result result = {0};
    result.index = -1;

    size_t size = 0;
    String cmd = {0};
    str_append_cstr(arena, &cmd, "echo \"");
    for (size_t i = 0; i < windows->count; ++i) {
        if (windows->items[i].name.size > size) {
            size = windows->items[i].name.size;
        }
        str_append_sv(arena, &cmd, windows->items[i].name);
        if (i < windows->count - 1) {
            str_append_cstr(arena, &cmd, "\\n");
        }
    }
    str_append_cstr(arena, &cmd, "\" | ");
    str_append_cstr(arena, &cmd, "dmenu -i -p \"Window to bring back from the Shadow Realm\"");
    str_append_null(arena, &cmd);

    printf("Executing the folowing command:\n");
    printf("%s\n", cmd.items);

    FILE *out = popen(cmd.items, "r");
    if (out == NULL) {
        result.failed = true;
        result.error = strerror(errno);
        goto cleanup;
    }

    char *selected_window = arena_alloc(arena, size + 1);
    memset(selected_window, 0, size + 1);

    char ch = getc(out);
    if (ch == EOF) {
        // NOTE(nic): user closed dmenu without selecting a window
        goto cleanup;
    }
    for (size_t i = 0; ch != '\n' && ch != '\0'; ++i) {
        selected_window[i] = ch;
        ch = getc(out);
    }

    String_View sv = SV(selected_window);
    for (size_t i = 0; i < windows->count; ++i) {
        if (sv_eq(windows->items[i].name, sv)) {
            result.index = i;
            goto cleanup;
        }
    }

cleanup:
    if (out != NULL) {
        pclose(out);
    }
    return result;
}

void show_notification(Arena *arena, const char *message) {
    String cmd = {0};
    str_append_fmt(arena, &cmd, "dunstify 'dmenu_scratchpad' '%s' -t 2000", message);
    str_append_null(arena, &cmd);
    // NOTE(nic): we don't particularly care if this fails
    system(cmd.items);
}

int main(void) {
    const char *socket_path = getenv("I3SOCK");
    if (socket_path == NULL) {
        fprintf(stderr, "Error: could not find i3 socket path\n");
        exit(1);
    }
    printf("Socket path: %s\n", socket_path);

    int socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        fprintf(stderr, "Error: could not create socket\n");
        exit(1);
    }

    struct sockaddr_un sockaddr = {0};
    sockaddr.sun_family = AF_UNIX;
    strcpy(sockaddr.sun_path, socket_path);

    int conn_success = connect(socket_fd, (struct sockaddr*)&sockaddr, sizeof(sockaddr));
    if (conn_success < 0) {
        fprintf(stderr, "Error: could not connect to i3: %s\n", strerror(errno));
        exit(1);
    }

    Arena arena = {0};
    {
        String packet = {0};
        str_append_cstr(&arena, &packet, I3_MAGIC);
        str_append_uint32_bytes_le(&arena, &packet, 0);
        str_append_uint32_bytes_le(&arena, &packet, 4);

        ssize_t bytes_sent = send(socket_fd, packet.items, packet.count, 0);
        if (bytes_sent < 0) {
            fprintf(stderr, "Error: could not send message to i3: %s\n", strerror(errno));
            exit(1);
        }
    }

    Windows windows = {0};
    size_t chosen_window_index = {0};
    {
        Json_Object json = {0};
        Json_Result result = i3_receive_message(&arena, socket_fd, &json);
        if (result.failed) {
            fprintf(stderr, "Json parser error at %zu: %s\n", result.error_loc, result.error);
            exit(1);
        }

        assert(json.kind == JSON_OBJ_DICT);
        Json_Dict *dict = &json.as.dict;

        Json_Array *nodes = json_dict_get_array(dict, json_obj_string(&arena, "nodes"));
        if (nodes == NULL) {
            fprintf(stderr, "Error: could not find i3 nodes\n");
            exit(1);
        }

        Json_Dict *scratchpad = i3_find_scratchpad(&arena, nodes);
        if (scratchpad == NULL) {
            fprintf(stderr, "Error: could not find i3 scratchpad\n");
            exit(1);
        }

        windows = i3_get_scratchpad_windows(&arena, scratchpad);
        if (windows.count <= 0) {
            show_notification(&arena, "Scratchpad is empty");
            exit(0);
        }

        Prompt_Result prompt_result = prompt_user(&arena, &windows);
        if (prompt_result.failed) {
            fprintf(stderr, "Error: dmenu call failed: %s\n", prompt_result.error);
            exit(1);
        }
        if (prompt_result.index < 0) {
            // NOTE(nic): user closed dmenu without selecting any window
            exit(0);
        }
        chosen_window_index = (size_t) prompt_result.index;
    }

    {
        Window chosen_window = windows.items[chosen_window_index];
        String command = {0};
        str_append_fmt(&arena, &command, "[con_id=\"%zu\"] focus", chosen_window.id);
        str_append_null(&arena, &command);

        String packet = {0};
        str_append_cstr(&arena, &packet, I3_MAGIC);
        str_append_uint32_bytes_le(&arena, &packet, (uint32_t)command.count - 1);
        str_append_uint32_bytes_le(&arena, &packet, 0);
        str_append_cstr(&arena, &packet, command.items);

        printf("Sending following message:\n");
        printf("%s\n", command.items);

        ssize_t bytes_sent = send(socket_fd, packet.items, packet.count, 0);
        if (bytes_sent < 0) {
            fprintf(stderr, "Error: could not send message to i3: %s\n", strerror(errno));
            exit(1);
        }
    }

    {
        Json_Object json = {0};
        Json_Result result = i3_receive_message(&arena, socket_fd, &json);
        if (result.failed) {
            fprintf(stderr, "Json parser error at %zu: %s\n", result.error_loc, result.error);
            exit(1);
        }

        assert(json.kind == JSON_OBJ_ARRAY);
        Json_Array *array = &json.as.array;

        Json_Dict *dict = json_array_get_dict(array, 0);
        bool *success = json_dict_get_boolean(dict, json_obj_string(&arena, "success"));
        if (success == NULL) {
            fprintf(stderr, "Error: could not find `success` entry in i3 response\n");
            exit(1);
        }

        if (!(*success)) {
            String *error = json_dict_get_string(dict, json_obj_string(&arena, "error"));
            bool *parse_error = json_dict_get_boolean(dict, json_obj_string(&arena, "parse_error"));
            if (parse_error != NULL && *parse_error) {
                String *input = json_dict_get_string(dict, json_obj_string(&arena, "input"));
                String *pos = json_dict_get_string(dict, json_obj_string(&arena, "errorposition"));
                fprintf(
                    stderr, "Error: i3 could not parse command: %.*s\n",
                    (int)error->count, error->items);
                fprintf(
                    stderr, "Input: %.*s\n       %.*s\n",
                    (int)input->count, input->items,
                    (int)pos->count, pos->items);
            } else {
                fprintf(
                    stderr, "Error: i3 could not execute command: %.*s\n",
                    (int)error->count, error->items);
            }
        }
    }

    arena_free(&arena);
    close(socket_fd);
    return 0;
}
