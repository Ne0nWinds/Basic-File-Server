
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

#define read_only __declspec(allocate(".roglob"))
#define len(arr) (sizeof(arr) / sizeof(*arr))
#define string8_static(str) (string8){ str, len(str) - 1 }

typedef struct {
	char *data;
	u32 length;
} string8;

typedef struct string8_builder_node string8_builder_node;
struct string8_builder_node {
	string8 value;
	string8_builder_node *next;
};

typedef struct {
	string8_builder_node *head;
	string8_builder_node *tail;
	u32 nodes;
	u32 total_string_length;
} string8_builder;

typedef struct {
	void *base;
	u64 offset;
	u64 capacity;
} memory_arena;


#define KB(b) (b * 1024ULL)
#define MB(b) (KB(b) * 1024ULL)
#define GB(b) (MB(b) * 1024ULL)
#define TB(b) (TB(b) * 1024ULL)

static inline void socket_close(int socket);
static memory_arena alloc_memory_arena_from_os(u64 size);
static string8_builder new_string8_builder(memory_arena *arena, string8 first);
static void string8_builder_append(string8_builder *builder, memory_arena *arena, string8 s);
static string8 string8_builder_finalize(string8_builder *builder, memory_arena *arena);

typedef struct {
	string8 file_data;
	u8 _opaque_os_data[16];
} file_mapping;
static file_mapping open_memory_mapped_file(memory_arena *arena, string8 filepath);
static void close_memory_mapped_file(file_mapping *file);

#if defined(_WIN32)

#include <Windows.h>
#define assert(expr) if (!(expr)) {\
	printf("\nAssertion failed: %s:%llu in function %s \n%s\n\n", __FILE__, (long long unsigned)__LINE__, __FUNCTION__, #expr);\
	__debugbreak();\
	ExitProcess(-1);\
}
static memory_arena alloc_memory_arena_from_os(u64 size) {
	memory_arena result = {0};
	result.base = VirtualAlloc(0, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	result.offset = 0;
	result.capacity = size;
	return result;
}
static inline void socket_close(int socket) {
	closesocket(socket);
}
static file_mapping open_memory_mapped_file(memory_arena *arena, string8 filepath) {

	string8 windows_file_path = {0};
	{
		string8_builder file_path_builder = new_string8_builder(arena, string8_static("."));
		string8_builder_append(&file_path_builder, arena, filepath);
		string8_builder_append(&file_path_builder, arena, string8_static("\0"));
		windows_file_path = string8_builder_finalize(&file_path_builder, arena);
	}

	file_mapping result = {0};

	HANDLE file_handle = CreateFileA(windows_file_path.data, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file_handle == INVALID_HANDLE_VALUE) return (file_mapping){0};
	((HANDLE *)result._opaque_os_data)[0] = file_handle;

	LARGE_INTEGER file_size = {0};
	if (!GetFileSizeEx(file_handle, &file_size)) goto error;

	HANDLE win32_file_mapping = CreateFileMappingA(file_handle, NULL, PAGE_READONLY, file_size.HighPart, file_size.LowPart, NULL);
	if (win32_file_mapping == NULL) goto error;
	((HANDLE *)result._opaque_os_data)[1] = win32_file_mapping;

	void *data = MapViewOfFile(win32_file_mapping, FILE_MAP_READ, 0, 0, file_size.QuadPart);
	if (!data) goto error;
	
	result.file_data = (string8){ .data = data, .length = file_size.QuadPart };
	return result;
error:
	close_memory_mapped_file(&result);
	return (file_mapping){0};
}
static void close_memory_mapped_file(file_mapping *file) {
	UnmapViewOfFile(file->file_data.data);
	CloseHandle(((HANDLE *)file->_opaque_os_data)[0]);
	CloseHandle(((HANDLE *)file->_opaque_os_data)[1]);
}

#else
#define assert(expr) if (!(expr)) {\
	printf("\nAssertion failed: %s:%llu in function %s \n%s\n\n", __FILE__, (long long unsigned)__LINE__, __FUNCTION__, #expr);\
	exit(-1);\
}
static memory_arena alloc_memory_arena_from_os(u64 size) {
	memory_arena result = {0};
	result.base = mmap(0, size, PROT_READ | PROT_WRITE, MAP_POPULATE | MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	result.offset = 0;
	result.capacity = size;
	return result;
}
static inline void socket_close(int socket) {
	close(socket);
}
#endif


static inline void *arena_push_aligned(memory_arena *arena, u32 size, u32 alignment) {
	assert(arena->offset + size < arena->capacity); 
	void *result = (u8 *)arena->base + arena->offset;
	memset(result, 0, size);
	arena->offset += size;
	return result;
}
static inline void *arena_push(memory_arena *arena, u32 size) {
	return arena_push_aligned(arena, size, 16);
}
static memory_arena arena_scratch(memory_arena *arena) {
	memory_arena result = {0};
	result.base = (u8 *)arena->base + arena->offset;
	result.offset = 0;
	result.capacity = arena->capacity - arena->offset;
	return result;
}

static memory_arena alloc_memory_arena_from_os(u64 size);


static inline string8 null_string() {
	return (string8){0};
}

static inline string8 string8_null_terminate(memory_arena *arena, string8 str) {
	string8 result = {0};
	result.data = arena_push(arena, str.length + 1);
	result.length = str.length + 1;
	memcpy(result.data, str.data, str.length);
	return result;
}

static char string8_pop_char(string8 *s) {
	if (s->length == 0) return 0;
	char result = *s->data;
	s->data += 1;
	s->length -= 1;
	return result;
}

static char string8_safe_peek(string8 *s) {
	if (s->length == 0) return 0;
	return s->data[0];
}

static void string8_pop_expected_string(string8 *s, string8 expected) {
	if (s->length < expected.length) {
		*s = null_string();
		return;
	}

	const u32 expected_length = expected.length;
	for (u32 i = 0; i < expected_length; ++i) {
		char expected_char = expected.data[i];
		char actual_char = string8_pop_char(s);
		if (expected_char != actual_char) {
			*s = null_string();
		}
	}
}
static void string8_pop_expected_char(string8 *s, char expected) {
	char actual = string8_pop_char(s);
	if (expected != actual) {
		*s = null_string();
	}
}

static string8 string8_pop_and_collect_string(string8 *s, char string_terminator, bool allow_end_of_string) {

	if (s->length == 0) goto error;

	string8 result = {0};
	result.data = s->data;
	result.length = 0;

	char c = string8_safe_peek(s);
	while (c != 0 && c != string_terminator) {
		result.length += 1;
		string8_pop_char(s);
		c = string8_safe_peek(s);
	}

	if (!allow_end_of_string && c == 0) goto error;

	return result;
error:
	*s = null_string();
	return null_string();
}

// TODO: perhaps we should pad strings for SIMD?
static bool string8_equal_to(string8 a, string8 b) {
	if (a.length != b.length) return false;
	const u32 length = b.length;
	for (u32 i = 0; i < length; ++i) {
		char a_char = a.data[i];
		char b_char = b.data[i];
		if (a_char != b_char) {
			return false;
		}
	}
	return true;
}

static string8 string8_from_u32(memory_arena *arena, u32 value) {

	char temp_buffer[32];

	u32 length = 0;
	do {
		char c = (value % 10) + '0';
		value /= 10;
		temp_buffer[15 - length] = c;
		length += 1;
	} while (value != 0);

	char *buffer = arena_push(arena, 16); // max size of u32 string is 10

	char *src = temp_buffer + 16 - length;
	for (u32 i = 0; i < 16; ++i) {
		buffer[i] = src[i];
	}

	string8 result = {0};
	result.data = buffer;
	result.length = length;

	return result;
}

static string8_builder new_string8_builder(memory_arena *arena, string8 first) {

	string8_builder_node *first_node = arena_push(arena, sizeof(string8_builder_node));
	first_node->value = first;
	first_node->next = 0;

	string8_builder result = {0};
	result.head = first_node;
	result.tail = first_node;
	result.nodes = 1;
	result.total_string_length = first.length;
	return result;
}

static void string8_builder_append(string8_builder *builder, memory_arena *arena, string8 s) {
	string8_builder_node *new_node = arena_push(arena, sizeof(string8_builder_node));
	new_node->value = s;
	new_node->next = 0;

	builder->tail->next = new_node;
	builder->tail = new_node;
	builder->nodes += 1;
	builder->total_string_length += s.length;
}

static string8 string8_builder_finalize(string8_builder *builder, memory_arena *arena) {
	string8 result = {0};
	result.data = arena_push(arena, builder->total_string_length);
	result.length = builder->total_string_length;

	char *dst = result.data;

	const u32 node_count = builder->nodes;
	string8_builder_node *node = builder->head;
	for (u32 i = 0; i < node_count; ++i) {
		memcpy(dst, node->value.data, node->value.length);
		dst += node->value.length;
		node = node->next;
	}

	return result;
}

typedef struct {
	string8 path;
} http_request;

static http_request parse_http_request(string8 request) {
	http_request result = {0};

	string8_pop_expected_string(&request, string8_static("GET "));
	result.path = string8_pop_and_collect_string(&request, ' ', false);
	string8_pop_expected_string(&request, string8_static(" HTTP/1.1\r\n"));

	return result;
}

typedef enum {
	HTTP_CONTENT_TYPE_DEFAULT = 0,
	HTTP_CONTENT_TYPE_HTML,
	HTTP_CONTENT_TYPE_CSS,
	HTTP_CONTENT_TYPE_JS,
	HTTP_CONTENT_TYPE_WASM
} http_content_type;

static http_content_type parse_content_type(string8 file_name) {
	string8_pop_and_collect_string(&file_name, '.', false);

	string8 file_extension = null_string();
	while (string8_pop_char(&file_name) == '.') {
		file_extension = string8_pop_and_collect_string(&file_name, '.', true);
	}

	if (string8_equal_to(file_extension, string8_static("html"))) {
		return HTTP_CONTENT_TYPE_HTML;
	}
	if (string8_equal_to(file_extension, string8_static("css"))) {
		return HTTP_CONTENT_TYPE_CSS;
	}
	if (string8_equal_to(file_extension, string8_static("js"))) {
		return HTTP_CONTENT_TYPE_JS;
	}
	if (string8_equal_to(file_extension, string8_static("wasm"))) {
		return HTTP_CONTENT_TYPE_WASM;
	}

	return HTTP_CONTENT_TYPE_DEFAULT;
}

int main() {
	memory_arena temp = alloc_memory_arena_from_os(MB(32));

#ifdef _WIN32
	{
		static struct WSAData wsa_data;
		int result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
		assert(result == 0);
	}
#endif

	int listen_fd = 0;
	{
		listen_fd = socket(AF_INET, SOCK_STREAM, 0);
		assert(listen_fd != 0);

		int options;
#if defined(_WIN32)
		options = SO_REUSEADDR;
#else
		options = SO_REUSEADDR | SO_REUSEPORT;
#endif
		setsockopt(listen_fd, SOL_SOCKET, options, (void *)&options, sizeof(options));

		struct sockaddr_in bind_info = {0};
		bind_info.sin_family = AF_INET;
		bind_info.sin_addr.s_addr = htonl(INADDR_ANY);
		bind_info.sin_port = htons(8000);

		int success;
		success = bind(listen_fd, (struct sockaddr *)&bind_info, sizeof(bind_info));
		assert(success == 0);

		success = listen(listen_fd, 1);
		assert(success == 0);
	}
	puts("Starting server on port :8000");

	for (;;) {
		memory_arena scratch = arena_scratch(&temp);

		struct sockaddr_in accept_info = {0};
		s32 accept_info_len = sizeof(accept_info);
		int new_connection = accept(listen_fd, (struct sockaddr *)&accept_info, &accept_info_len);

		if (new_connection == -1) break;

		const u32 recv_buffer_max_size = 2047;
		void *recv_buffer = arena_push(&scratch, recv_buffer_max_size + 1);
		int data_received = recv(new_connection, recv_buffer, recv_buffer_max_size, 0);

		if (data_received == -1 || data_received == 0) {
			socket_close(new_connection);
			puts("Invalid HTTP request");
			continue;
		}

		http_request request = parse_http_request((string8){ recv_buffer, data_received });

		string8 path = request.path;
		if (string8_equal_to(path, string8_static("/"))) {
			path = string8_static("/index.html");
		}

		file_mapping __attribute__((cleanup(close_memory_mapped_file))) mapped_file = open_memory_mapped_file(&scratch, path);

		if (mapped_file.file_data.length == 0) {
			string8 response = string8_static(
				"HTTP/1.1 404 NotFound\r\n"
				"Content-Type: text/html; charset=UTF-8\r\n"
				"Content-Length: 50\r\n"
				"\r\n"
				"<!DOCTYPE html><html><h1>404 Not Found</h1></html>"
			);
			send(new_connection, response.data, response.length, 0);
			socket_close(new_connection);
			continue;
		}

		{
			string8_builder response_builder = new_string8_builder(&scratch,
				string8_static(
					"HTTP/1.1 200 OK\r\n"
					"Cross-Origin-Opener-Policy: same-origin\r\n"
					"Cross-Origin-Embedder-Policy: require-corp\r\n"
				)
			);

			string8 new_line = string8_static("\r\n");

			string8 content_type_header = string8_static("Content-Type: ");
			string8 content_type_value;
			http_content_type content_type = parse_content_type(path);
			switch (content_type) {
				case HTTP_CONTENT_TYPE_HTML: {
					content_type_value = string8_static("text/html; charset=UTF-8");
				} break;
				case HTTP_CONTENT_TYPE_JS: {
					content_type_value = string8_static("text/javascript; charset=UTF-8");
				} break;
				case HTTP_CONTENT_TYPE_WASM: {
					content_type_value = string8_static("application/wasm");
				} break;
				default: {
					content_type_value = string8_static("application/octet-stream");
				}
			}

			string8_builder_append(&response_builder, &scratch, content_type_header);
			string8_builder_append(&response_builder, &scratch, content_type_value);
			string8_builder_append(&response_builder, &scratch, new_line);

			string8 content_length_header = string8_static("Content-Length: ");
			string8_builder_append(&response_builder, &scratch, content_length_header);

			string8_builder_append(&response_builder, &scratch, string8_from_u32(&scratch, mapped_file.file_data.length));
			string8_builder_append(&response_builder, &scratch, new_line);
			string8_builder_append(&response_builder, &scratch, new_line);

			string8 response_header = string8_builder_finalize(&response_builder, &scratch);
			send(new_connection, response_header.data, response_header.length, 0);
			send(new_connection, mapped_file.file_data.data, mapped_file.file_data.length, 0);
		}

		socket_close(new_connection);
	}

	puts("Closing server");
	return 0;
}
