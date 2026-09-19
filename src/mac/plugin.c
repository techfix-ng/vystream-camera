#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <arpa/inet.h>
#include <dlfcn.h>
#include <netdb.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <AudioToolbox/AudioToolbox.h>

typedef struct obs_module obs_module_t;
typedef struct obs_data obs_data_t;
typedef struct obs_source obs_source_t;
typedef struct obs_scene obs_scene_t;
typedef struct obs_sceneitem obs_sceneitem_t;
static obs_module_t *module_pointer;
static volatile bool running;
static volatile bool shutting_down;
static int discovery_socket = -1;
static pthread_t worker;
static bool worker_started;
static int return_socket = -1;
static pthread_t return_worker;
static bool return_worker_started;
static AudioQueueRef reply_output;
typedef struct {
	char id[96], name[128], scene[128], ip[64], token[17];
	int port;
} TalkbackTarget;
static TalkbackTarget talkback_targets[128];
static int talkback_count;
static int talkback_selected = -1;
static AudioQueueRef talkback_queue;
static volatile bool talking;
#ifdef VYSTRM_QT_DOCK
#ifdef __cplusplus
extern "C" void vystrm_register_dock(void);
#else
extern void vystrm_register_dock(void);
#endif
#endif
extern bool vystrm_auth_is_authenticated(void);
extern int vystrm_auth_max_cameras(void);
extern int vystrm_auth_max_width(void);
extern int vystrm_auth_max_height(void);

static void send_talkback(const void *bytes, size_t length)
{
	for (int i = 0; i < talkback_count; i++) {
		if (talkback_selected >= 0 && talkback_selected != i)
			continue;
		char packet[1500];
		int h = snprintf(packet, sizeof(packet), "VYSTB1|%s|", talkback_targets[i].token);
		if (h < 0 || h + (int)length > (int)sizeof(packet))
			continue;
		memcpy(packet + h, bytes, length);
		struct sockaddr_in to = {0};
		to.sin_family = AF_INET;
		to.sin_port = htons(talkback_targets[i].port);
		if (inet_pton(AF_INET, talkback_targets[i].ip, &to.sin_addr) == 1)
			sendto(discovery_socket, packet, h + length, 0, (struct sockaddr *)&to, sizeof(to));
	}
}
static void audio_input(void *user, AudioQueueRef q, AudioQueueBufferRef b, const AudioTimeStamp *t, UInt32 n,
			const AudioStreamPacketDescription *p)
{
	(void)user;
	(void)t;
	(void)n;
	(void)p;
	if (talking && b->mAudioDataByteSize)
		send_talkback(b->mAudioData, b->mAudioDataByteSize);
	if (talking) {
		b->mAudioDataByteSize = 0;
		AudioQueueEnqueueBuffer(q, b, 0, NULL);
	}
}
static bool start_talkback(void)
{
	if (talking)
		return true;
	AudioStreamBasicDescription f = {0};
	f.mSampleRate = 16000;
	f.mFormatID = kAudioFormatLinearPCM;
	f.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
	f.mBytesPerPacket = 640;
	f.mFramesPerPacket = 320;
	f.mBytesPerFrame = 2;
	f.mChannelsPerFrame = 1;
	f.mBitsPerChannel = 16;
	if (AudioQueueNewInput(&f, audio_input, NULL, NULL, NULL, 0, &talkback_queue) != noErr)
		return false;
	talking = true;
	for (int i = 0; i < 4; i++) {
		AudioQueueBufferRef b;
		if (AudioQueueAllocateBuffer(talkback_queue, 640, &b) != noErr)
			continue;
		AudioQueueEnqueueBuffer(talkback_queue, b, 0, NULL);
	}
	return AudioQueueStart(talkback_queue, NULL) == noErr;
}
static void stop_talkback(void)
{
	if (!talking)
		return;
	talking = false;
	if (talkback_queue) {
		AudioQueueStop(talkback_queue, true);
		AudioQueueDispose(talkback_queue, true);
		talkback_queue = NULL;
	}
}
static bool reply_token_selected(const char *token)
{
	for (int i = 0; i < talkback_count; i++)
		if (!strcmp(talkback_targets[i].token, token))
			return talkback_selected < 0 || talkback_selected == i;
	return false;
}
static void relay_crew_reply(const char *sender_token, const void *bytes, size_t length)
{
	int sender = -1;
	for (int i = 0; i < talkback_count; i++)
		if (!strcmp(talkback_targets[i].token, sender_token)) {
			sender = i;
			break;
		}
	if (sender < 0)
		return;
	for (int i = 0; i < talkback_count; i++) {
		if (i == sender)
			continue;
		char packet[1500];
		int h = snprintf(packet, sizeof(packet), "VYSTB2|%s|%s|", talkback_targets[i].token,
				 talkback_targets[sender].name);
		if (h < 0 || h + (int)length > (int)sizeof(packet))
			continue;
		memcpy(packet + h, bytes, length);
		struct sockaddr_in to = {0};
		to.sin_family = AF_INET;
		to.sin_port = htons(talkback_targets[i].port);
		if (inet_pton(AF_INET, talkback_targets[i].ip, &to.sin_addr) == 1)
			sendto(discovery_socket, packet, h + length, 0, (struct sockaddr *)&to, sizeof(to));
	}
}
static void reply_buffer_done(void *u, AudioQueueRef q, AudioQueueBufferRef b)
{
	(void)u;
	AudioQueueFreeBuffer(q, b);
}
static void play_reply(const void *bytes, size_t length)
{
	if (!reply_output || !length)
		return;
	AudioQueueBufferRef b;
	if (AudioQueueAllocateBuffer(reply_output, (UInt32)length, &b) != noErr)
		return;
	memcpy(b->mAudioData, bytes, length);
	b->mAudioDataByteSize = (UInt32)length;
	AudioQueueEnqueueBuffer(reply_output, b, 0, NULL);
}
static void *return_loop(void *unused)
{
	(void)unused;
	char b[1500];
	while (running) {
		struct sockaddr_in from;
		socklen_t z = sizeof(from);
		ssize_t n = recvfrom(return_socket, b, sizeof(b), 0, (struct sockaddr *)&from, &z);
		if (n < 10) {
			if (!running)
				break;
			continue;
		}
		if (memcmp(b, "VYSUP1|", 7))
			continue;
		int end = 7;
		while (end < n && b[end] != '|')
			end++;
		if (end >= n || end - 7 > 16)
			continue;
		char token[17] = {0};
		memcpy(token, b + 7, end - 7);
		relay_crew_reply(token, b + end + 1, n - end - 1);
		if (reply_token_selected(token))
			play_reply(b + end + 1, n - end - 1);
	}
	return NULL;
}
static void start_return_listener(void)
{
	return_socket = socket(AF_INET, SOCK_DGRAM, 0);
	if (return_socket < 0)
		return;
	int yes = 1;
	setsockopt(return_socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	struct sockaddr_in a = {0};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	a.sin_port = htons(46011);
	if (bind(return_socket, (struct sockaddr *)&a, sizeof(a)) < 0) {
		close(return_socket);
		return_socket = -1;
		return;
	}
	AudioStreamBasicDescription f = {0};
	f.mSampleRate = 16000;
	f.mFormatID = kAudioFormatLinearPCM;
	f.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
	f.mBytesPerPacket = 2;
	f.mFramesPerPacket = 1;
	f.mBytesPerFrame = 2;
	f.mChannelsPerFrame = 1;
	f.mBitsPerChannel = 16;
	if (AudioQueueNewOutput(&f, reply_buffer_done, NULL, NULL, NULL, 0, &reply_output) != noErr) {
		close(return_socket);
		return_socket = -1;
		return;
	}
	AudioQueueStart(reply_output, NULL);
	if (pthread_create(&return_worker, NULL, return_loop, NULL) == 0)
		return_worker_started = true;
}
static void stop_return_listener(void)
{
	if (return_socket >= 0) {
		shutdown(return_socket, SHUT_RDWR);
		close(return_socket);
		return_socket = -1;
	}
	if (return_worker_started) {
		pthread_join(return_worker, NULL);
		return_worker_started = false;
	}
	if (reply_output) {
		AudioQueueStop(reply_output, true);
		AudioQueueDispose(reply_output, true);
		reply_output = NULL;
	}
}
static void show_talkback(void *unused)
{
	(void)unused;
	if (talkback_count == 0) {
		system("/usr/bin/osascript -e 'display alert \"No VyStream cameras online\" message \"Open VyStream on the phone and run Wi-Fi discovery first.\"'");
		return;
	}
	char command[8192] = "/usr/bin/osascript -e 'choose from list {\"All paired cameras\"";
	for (int i = 0; i < talkback_count; i++) {
		strncat(command, ",\"", sizeof(command) - strlen(command) - 1);
		strncat(command, talkback_targets[i].name, sizeof(command) - strlen(command) - 1);
		strncat(command, "\"", sizeof(command) - strlen(command) - 1);
	}
	strncat(command, "} with title \"VyStream Director Talkback\" with prompt \"Send talkback to:\"' 2>/dev/null",
		sizeof(command) - strlen(command) - 1);
	FILE *p = popen(command, "r");
	char chosen[256] = {0};
	if (p) {
		fgets(chosen, sizeof(chosen), p);
		pclose(p);
	}
	chosen[strcspn(chosen, "\r\n")] = 0;
	if (!chosen[0] || !strcmp(chosen, "false"))
		return;
	talkback_selected = -1;
	for (int i = 0; i < talkback_count; i++)
		if (!strcmp(chosen, talkback_targets[i].name))
			talkback_selected = i;
	if (!start_talkback()) {
		system("/usr/bin/osascript -e 'display alert \"Microphone unavailable\" message \"Allow OBS microphone access in System Settings > Privacy & Security.\"'");
		return;
	}
	system("/usr/bin/osascript -e 'display dialog \"Director talkback is live.\" buttons {\"Stop Talkback\"} default button \"Stop Talkback\" with title \"VyStream Director Talkback\"'");
	stop_talkback();
}
static void register_talkback_menu(void)
{
#ifdef VYSTRM_QT_DOCK
	vystrm_register_dock();
#else
	void (*add)(const char *, void (*)(void *), void *) = dlsym(RTLD_DEFAULT, "obs_frontend_add_tools_menu_item");
	if (add)
		add("VyStream Director Talkback", show_talkback, NULL);
#endif
}

typedef struct {
	char id[96];
	char scene_name[128];
	char source_name[128];
	int port;
	char token[17];
} Device;
typedef struct {
	char scene_name[128];
	char source_name[128];
	int port;
} SourceRequest;

static void clean_field(char *text)
{
	for (char *p = text; *p; ++p)
		if (*p == '|' || *p == '\t' || *p == '\r' || *p == '\n' || *p == '\'' || *p == '"' || *p == '\\' ||
		    *p == '`')
			*p = '-';
}
static const char *config_path(void)
{
	static char path[1024];
	const char *home = getenv("HOME");
	if (!home)
		home = "/tmp";
	snprintf(path, sizeof(path),
		 "%s/Library/Application Support/obs-studio/plugin_config/obs-srt-camera/devices.tsv", home);
	return path;
}
static void ensure_config_dir(void)
{
	const char *home = getenv("HOME");
	if (!home)
		return;
	char p[1024];
	snprintf(p, sizeof(p), "%s/Library/Application Support/obs-studio", home);
	mkdir(p, 0755);
	strncat(p, "/plugin_config", sizeof(p) - strlen(p) - 1);
	mkdir(p, 0755);
	strncat(p, "/obs-srt-camera", sizeof(p) - strlen(p) - 1);
	mkdir(p, 0755);
}
static int load_devices(Device *items, int capacity)
{
	FILE *f = fopen(config_path(), "r");
	if (!f)
		return 0;
	int n = 0;
	char line[512];
	while (n < capacity && fgets(line, sizeof(line), f)) {
		memset(&items[n], 0, sizeof(Device));
		int fields = sscanf(line, "%95[^\t]\t%127[^\t]\t%127[^\t]\t%d\t%16s", items[n].id, items[n].scene_name,
				    items[n].source_name, &items[n].port, items[n].token);
		if (fields >= 4)
			n++;
	}
	fclose(f);
	return n;
}
static bool unique_names(const char *scene, const char *source, const char *id)
{
	Device d[128];
	int n = load_devices(d, 128);
	for (int i = 0; i < n; i++)
		if (strcmp(d[i].id, id) &&
		    (!strcasecmp(d[i].scene_name, scene) || !strcasecmp(d[i].source_name, source)))
			return false;
	return true;
}
static bool find_device(const char *id, Device *out)
{
	Device d[128];
	int n = load_devices(d, 128);
	for (int i = n - 1; i >= 0; i--)
		if (strcmp(d[i].id, id) == 0) {
			*out = d[i];
			return true;
		}
	return false;
}
static int next_port(void)
{
	Device d[128];
	int n = load_devices(d, 128), p = 8999;
	for (int i = 0; i < n; i++)
		if (d[i].port > p)
			p = d[i].port;
	return p + 1;
}
static void save_device(const Device *d)
{
	ensure_config_dir();
	FILE *f = fopen(config_path(), "a");
	if (f) {
		fprintf(f, "%s\t%s\t%s\t%d\t%s\n", d->id, d->scene_name, d->source_name, d->port, d->token);
		fclose(f);
	}
}

static bool ask_scene_source_names(const char *suggested, const char *id, char *scene, size_t scene_size, char *source,
				   size_t source_size)
{
	char base[96];
	snprintf(base, sizeof(base), "%s", suggested && *suggested ? suggested : "VYSTRM Camera");
	clean_field(base);
	for (;;) {
		char command[1200], scene_default[128], source_default[128];
		snprintf(scene_default, sizeof(scene_default), "%s Scene", base);
		snprintf(source_default, sizeof(source_default), "%s-Cam", base);
		snprintf(
			command, sizeof(command),
			"/usr/bin/osascript -e 'text returned of (display dialog \"Enter the permanent OBS scene name for this camera:\" default answer \"%s\" with title \"VYSTRM Camera Setup\" buttons {\"Cancel\", \"Next\"} default button \"Next\")' 2>/dev/null",
			scene_default);
		FILE *pipe = popen(command, "r");
		if (!pipe)
			return false;
		bool ok = fgets(scene, scene_size, pipe) != NULL;
		int status = pclose(pipe);
		if (!ok || status != 0)
			return false;
		scene[strcspn(scene, "\r\n")] = 0;
		clean_field(scene);
		if (!*scene)
			continue;
		snprintf(
			command, sizeof(command),
			"/usr/bin/osascript -e 'text returned of (display dialog \"Enter the camera source name to place inside %s:\" default answer \"%s\" with title \"VYSTRM Camera Setup\" buttons {\"Cancel\", \"Create\"} default button \"Create\")' 2>/dev/null",
			scene, source_default);
		pipe = popen(command, "r");
		if (!pipe)
			return false;
		ok = fgets(source, source_size, pipe) != NULL;
		status = pclose(pipe);
		if (!ok || status != 0)
			return false;
		source[strcspn(source, "\r\n")] = 0;
		clean_field(source);
		if (!*source)
			continue;
		if (strcasecmp(scene, source) && unique_names(scene, source, id))
			return true;
		system("/usr/bin/osascript -e 'display alert \"Names already used\" message \"Choose different unique scene and source names.\" as warning' >/dev/null 2>&1");
	}
}
static void local_ip_for(const struct sockaddr_in *peer, char *out, size_t size)
{
	snprintf(out, size, "127.0.0.1");
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0)
		return;
	connect(s, (const struct sockaddr *)peer, sizeof(*peer));
	struct sockaddr_in local;
	socklen_t z = sizeof(local);
	if (getsockname(s, (struct sockaddr *)&local, &z) == 0)
		inet_ntop(AF_INET, &local.sin_addr, out, (socklen_t)size);
	close(s);
}
static void random_token(char *out, size_t size)
{
	unsigned char b[8];
	arc4random_buf(b, sizeof(b));
	for (int i = 0; i < 8; i++)
		snprintf(out + i * 2, size - i * 2, "%02x", b[i]);
}
static void computer_name(char *out, size_t size)
{
	if (gethostname(out, size) != 0)
		snprintf(out, size, "OBS-Mac");
	out[size - 1] = 0;
	clean_field(out);
}

static void wifi_ip(char *out, size_t size)
{
	out[0] = 0;
	struct ifaddrs *list = NULL;
	if (getifaddrs(&list) != 0)
		return;
	for (struct ifaddrs *a = list; a; a = a->ifa_next) {
		if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || !(a->ifa_flags & IFF_UP) || !a->ifa_name)
			continue;
		if (strcmp(a->ifa_name, "en0") && strcmp(a->ifa_name, "en1"))
			continue;
		struct sockaddr_in *s = (struct sockaddr_in *)a->ifa_addr;
		if (inet_ntop(AF_INET, &s->sin_addr, out, (socklen_t)size))
			break;
	}
	freeifaddrs(list);
}
static const char *desktop_token(void)
{
	static char value[17];
	if (value[0])
		return value;
	ensure_config_dir();
	char path[1200];
	snprintf(path, sizeof(path), "%s.desktop-token", config_path());
	FILE *f = fopen(path, "r");
	if (f) {
		fscanf(f, "%16s", value);
		fclose(f);
	}
	if (!value[0]) {
		random_token(value, sizeof(value));
		f = fopen(path, "w");
		if (f) {
			fprintf(f, "%s\n", value);
			fclose(f);
		}
	}
	return value;
}

#ifdef __cplusplus
extern "C" {
#endif
const char *vystrm_pairing_payload(void)
{
	static char payload[512], ip[64], host[128];
	wifi_ip(ip, sizeof(ip));
	if (!ip[0])
		return "";
	computer_name(host, sizeof(host));
	snprintf(payload, sizeof(payload), "vys://p?i=%s&p=9000&t=%s&n=VYSTRM-%s", ip, desktop_token(), host);
	return payload;
}
int vystrm_camera_count(void)
{
	return talkback_count;
}
const char *vystrm_camera_name(int index)
{
	return index >= 0 && index < talkback_count ? talkback_targets[index].name : "";
}
const char *vystrm_camera_health(int index)
{
	static char value[256];
	if (index < 0 || index >= talkback_count) {
		value[0] = 0;
		return value;
	}
	snprintf(value, sizeof(value), "STATE CONNECTED\\nCAMERA %s", talkback_targets[index].name);
	return value;
}
void vystrm_set_camera_control(int index, const char *control, const char *value)
{
	if (shutting_down || !control || index < 0 || index >= talkback_count)
		return;
	char packet[1024];
	int length = snprintf(packet, sizeof(packet), "VYSCONTROL1|%s|%s|%s", talkback_targets[index].token,
				 control, value ? value : "");
	if (length <= 0 || length >= (int)sizeof(packet))
		return;
	struct sockaddr_in to = {0};
	to.sin_family = AF_INET;
	to.sin_port = htons(talkback_targets[index].port);
	if (inet_pton(AF_INET, talkback_targets[index].ip, &to.sin_addr) == 1)
		sendto(discovery_socket, packet, (size_t)length, 0, (struct sockaddr *)&to, sizeof(to));
}
bool vystrm_rename_camera(int index, const char *scene, const char *source)
{
	if (shutting_down || index < 0 || index >= talkback_count || !scene || !source ||
	    !*scene || !*source || !strcasecmp(scene, source) || !unique_names(scene, source, talkback_targets[index].id))
		return false;
	Device devices[128];
	int count = load_devices(devices, 128);
	for (int i = count - 1; i >= 0; i--) {
		if (strcmp(devices[i].id, talkback_targets[index].id))
			continue;
		Device updated = devices[i];
		snprintf(updated.scene_name, sizeof(updated.scene_name), "%s", scene);
		snprintf(updated.source_name, sizeof(updated.source_name), "%s", source);
		save_device(&updated);
		snprintf(talkback_targets[index].scene, sizeof(talkback_targets[index].scene), "%s", scene);
		snprintf(talkback_targets[index].name, sizeof(talkback_targets[index].name), "%s", source);
		void (*source_set_name)(obs_source_t *, const char *) = dlsym(RTLD_DEFAULT, "obs_source_set_name");
		obs_source_t *(*source_by_name)(const char *) = dlsym(RTLD_DEFAULT, "obs_get_source_by_name");
		void (*source_release)(obs_source_t *) = dlsym(RTLD_DEFAULT, "obs_source_release");
		if (source_set_name && source_by_name && source_release) {
			obs_source_t *old_scene = source_by_name(devices[i].scene_name);
			if (old_scene) {
				source_set_name(old_scene, scene);
				source_release(old_scene);
			}
			obs_source_t *old_source = source_by_name(devices[i].source_name);
			if (old_source) {
				source_set_name(old_source, source);
				source_release(old_source);
			}
		}
		void (*frontend_save)(void) = dlsym(RTLD_DEFAULT, "obs_frontend_save");
		if (frontend_save)
			frontend_save();
		return true;
	}
	return false;
}
void vystrm_select_camera(int index)
{
	talkback_selected = index;
}
bool vystrm_talkback_start(void)
{
	return start_talkback();
}
void vystrm_talkback_stop(void)
{
	stop_talkback();
}
#ifdef __cplusplus
}
#endif

static void create_source(void *parameter)
{
	SourceRequest req = *(SourceRequest *)parameter;
	free(parameter);
	if (shutting_down)
		return;
	obs_data_t *(*data_create)(void) = dlsym(RTLD_DEFAULT, "obs_data_create");
	void (*data_string)(obs_data_t *, const char *, const char *) = dlsym(RTLD_DEFAULT, "obs_data_set_string");
	void (*data_bool)(obs_data_t *, const char *, bool) = dlsym(RTLD_DEFAULT, "obs_data_set_bool");
	void (*data_release)(obs_data_t *) = dlsym(RTLD_DEFAULT, "obs_data_release");
	obs_source_t *(*source_create)(const char *, const char *, obs_data_t *, obs_data_t *) =
		dlsym(RTLD_DEFAULT, "obs_source_create");
	obs_source_t *(*source_by_name)(const char *) = dlsym(RTLD_DEFAULT, "obs_get_source_by_name");
	void (*source_release)(obs_source_t *) = dlsym(RTLD_DEFAULT, "obs_source_release");
	obs_scene_t *(*scene_from_source)(const obs_source_t *) = dlsym(RTLD_DEFAULT, "obs_scene_from_source");
	obs_sceneitem_t *(*scene_add)(obs_scene_t *, obs_source_t *) = dlsym(RTLD_DEFAULT, "obs_scene_add");
	obs_sceneitem_t *(*scene_find)(obs_scene_t *, const char *) = dlsym(RTLD_DEFAULT, "obs_scene_find_source");
	obs_scene_t *(*scene_create)(const char *) = dlsym(RTLD_DEFAULT, "obs_scene_create");
	void (*scene_release)(obs_scene_t *) = dlsym(RTLD_DEFAULT, "obs_scene_release");
	if (!data_create || !data_string || !data_bool || !data_release || !source_create || !source_by_name ||
	    !source_release || !scene_from_source || !scene_add || !scene_create || !scene_release)
		return;
	obs_source_t *scene_source = source_by_name(req.scene_name);
	obs_scene_t *scene = scene_source ? scene_from_source(scene_source) : scene_create(req.scene_name);
	if (!scene) {
		if (scene_source)
			source_release(scene_source);
		return;
	}
	obs_source_t *phone = source_by_name(req.source_name);
	if (!phone) {
		char input[256];
		snprintf(input, sizeof(input), "srt://0.0.0.0:%d?mode=listener&latency=200000", req.port);
		obs_data_t *d = data_create();
		data_bool(d, "is_local_file", false);
		data_string(d, "input", input);
		data_string(d, "input_format", "mpegts");
		data_bool(d, "restart_on_activate", true);
		data_bool(d, "close_when_inactive", false);
		phone = source_create("ffmpeg_source", req.source_name, d, NULL);
		data_release(d);
	}
	if (phone && (!scene_find || !scene_find(scene, req.source_name)))
		scene_add(scene, phone);
	if (phone)
		source_release(phone);
	if (scene_source)
		source_release(scene_source);
	else
		scene_release(scene);
	void (*frontend_save)(void) = dlsym(RTLD_DEFAULT, "obs_frontend_save");
	if (frontend_save)
		frontend_save();
}
static void queue_source(const char *scene, const char *source, int port)
{
	void (*queue)(int, void (*)(void *), void *, bool) = dlsym(RTLD_DEFAULT, "obs_queue_task");
	if (!queue)
		return;
	SourceRequest *r = calloc(1, sizeof(*r));
	snprintf(r->scene_name, sizeof(r->scene_name), "%s", scene);
	snprintf(r->source_name, sizeof(r->source_name), "%s", source);
	r->port = port;
	queue(0, create_source, r, false);
}

static void restore_saved_sources(void)
{
	Device devices[128];
	int count = load_devices(devices, 128);
	for (int i = 0; i < count; i++)
		if (devices[i].scene_name[0] && devices[i].source_name[0] && devices[i].port > 0)
			queue_source(devices[i].scene_name, devices[i].source_name, devices[i].port);
}

static void *discovery_loop(void *unused)
{
	(void)unused;
	char buffer[1024];
	while (running) {
		struct sockaddr_in sender;
		socklen_t z = sizeof(sender);
		ssize_t n = recvfrom(discovery_socket, buffer, sizeof(buffer) - 1, 0, (struct sockaddr *)&sender, &z);
		if (n < 0) {
			if (!running)
				break;
			continue;
		}
		buffer[n] = 0;
		char *save = NULL, *kind = strtok_r(buffer, "|", &save), *id = strtok_r(NULL, "|", &save),
		     *suggested = strtok_r(NULL, "|", &save), *talkback = strtok_r(NULL, "|", &save);
		if (!kind || !id || !suggested ||
		    (strcmp(kind, "OBS_SRT_DISCOVER_V3") && strcmp(kind, "OBS_SRT_DISCOVER_V4")))
			continue;
		clean_field(id);
		clean_field(suggested);
		if (!vystrm_auth_is_authenticated())
			continue;
		Device d = {0};
		if (!find_device(id, &d)) {
			if (talkback_count >= vystrm_auth_max_cameras())
				continue;
			snprintf(d.id, sizeof(d.id), "%s", id);
			d.port = next_port();
			if (!ask_scene_source_names(suggested, id, d.scene_name, sizeof(d.scene_name), d.source_name,
						    sizeof(d.source_name)))
				continue;
			random_token(d.token, sizeof(d.token));
			save_device(&d);
		}
		if (!d.token[0]) {
			random_token(d.token, sizeof(d.token));
			save_device(&d);
		}
		if (!strcmp(kind, "OBS_SRT_DISCOVER_V4")) {
			char phone[64];
			inet_ntop(AF_INET, &sender.sin_addr, phone, sizeof(phone));
			int tb = talkback ? atoi(talkback) : 46010;
			if (tb < 1024 || tb > 65535)
				tb = 46010;
			int slot = -1;
			for (int i = 0; i < talkback_count; i++)
				if (!strcmp(talkback_targets[i].id, id))
					slot = i;
			if (slot < 0 && talkback_count < 128)
				slot = talkback_count++;
			if (slot >= 0) {
				snprintf(talkback_targets[slot].id, sizeof(talkback_targets[slot].id), "%s", id);
				snprintf(talkback_targets[slot].name, sizeof(talkback_targets[slot].name), "%s",
					 d.source_name);
				snprintf(talkback_targets[slot].scene, sizeof(talkback_targets[slot].scene), "%s",
					 d.scene_name);
				snprintf(talkback_targets[slot].ip, sizeof(talkback_targets[slot].ip), "%s", phone);
				snprintf(talkback_targets[slot].token, sizeof(talkback_targets[slot].token), "%s",
					 d.token);
				talkback_targets[slot].port = tb;
			}
		}
		char host[256], ip[64], offer[1024];
		computer_name(host, sizeof(host));
		local_ip_for(&sender, ip, sizeof(ip));
		snprintf(offer, sizeof(offer), "OBS_SRT_OFFER_V4|%s|%s|%s|%d|%s|%s|macos|obs|3.0.4",
				 desktop_token(), host, ip, d.port, d.token, d.source_name);
		sendto(discovery_socket, offer, strlen(offer), 0, (struct sockaddr *)&sender, z);
		queue_source(d.scene_name, d.source_name, d.port);
	}
	return NULL;
}

void vystrm_send_tally_states(void)
{
	if (shutting_down || !running || discovery_socket < 0)
		return;
	obs_source_t *(*current_scene)(void) = dlsym(RTLD_DEFAULT, "obs_frontend_get_current_scene");
	obs_source_t *(*preview_scene)(void) = dlsym(RTLD_DEFAULT, "obs_frontend_get_current_preview_scene");
	const char *(*source_name)(const obs_source_t *) = dlsym(RTLD_DEFAULT, "obs_source_get_name");
	void (*source_release)(obs_source_t *) = dlsym(RTLD_DEFAULT, "obs_source_release");
	obs_source_t *program = current_scene ? current_scene() : NULL;
	obs_source_t *preview = preview_scene ? preview_scene() : NULL;
	char program_name[256] = {0}, preview_name[256] = {0};
	if (program && source_name)
		snprintf(program_name, sizeof(program_name), "%s", source_name(program));
	if (preview && source_name)
		snprintf(preview_name, sizeof(preview_name), "%s", source_name(preview));
	if (program && source_release)
		source_release(program);
	if (preview && source_release)
		source_release(preview);
	for (int i = 0; i < talkback_count; i++) {
		const TalkbackTarget *target = &talkback_targets[i];
		int state = 0;
		if (program_name[0] && ((target->scene[0] && strcasecmp(program_name, target->scene) == 0) ||
				(!target->scene[0] && (!target->name[0] || strcasecmp(program_name, target->name) == 0))))
			state = 1;
		else if (preview_name[0] && ((target->scene[0] && strcasecmp(preview_name, target->scene) == 0) ||
				(!target->scene[0] && (!target->name[0] || strcasecmp(preview_name, target->name) == 0))))
			state = 2;
		char packet[256];
		int length = snprintf(packet, sizeof(packet), "VYSTALLY1|%s|%d", target->token, state);
		struct sockaddr_in to = {0};
		to.sin_family = AF_INET;
		to.sin_port = htons(target->port);
		if (length > 0 && inet_pton(AF_INET, target->ip, &to.sin_addr) == 1)
			sendto(discovery_socket, packet, (size_t)length, 0, (struct sockaddr *)&to, sizeof(to));
	}
}

__attribute__((visibility("default"))) void obs_module_set_pointer(obs_module_t *m)
{
	module_pointer = m;
}
__attribute__((visibility("default"))) uint32_t obs_module_ver(void)
{
	uint32_t (*v)(void) = dlsym(RTLD_DEFAULT, "obs_get_version");
	return v ? v() : (30u << 24);
}
__attribute__((visibility("default"))) const char *obs_module_name(void)
{
	return "VYSTREAM Camera 3.0.4";
}
__attribute__((visibility("default"))) const char *obs_module_description(void)
{
	return "VYSTREAM Camera authentication, subscription entitlements, persistent OBS sources, tally, and talkback.";
}
__attribute__((visibility("default"))) const char *obs_module_author(void)
{
	return "TechFixNG";
}
__attribute__((visibility("default"))) bool obs_module_load(void)
{
	shutting_down = false;
	restore_saved_sources();
	register_talkback_menu();
	discovery_socket = socket(AF_INET, SOCK_DGRAM, 0);
	if (discovery_socket < 0)
		return true;
	int yes = 1;
	setsockopt(discovery_socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	struct sockaddr_in a = {0};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	a.sin_port = htons(45990);
	if (bind(discovery_socket, (struct sockaddr *)&a, sizeof(a)) < 0) {
		close(discovery_socket);
		discovery_socket = -1;
		return true;
	}
	running = true;
	start_return_listener();
	if (pthread_create(&worker, NULL, discovery_loop, NULL) != 0) {
		running = false;
		close(discovery_socket);
		discovery_socket = -1;
		stop_return_listener();
		return true;
	}
	worker_started = true;
	return true;
}
__attribute__((visibility("default"))) void obs_module_unload(void)
{
	shutting_down = true;
	stop_talkback();
	running = false;
	if (discovery_socket >= 0) {
		shutdown(discovery_socket, SHUT_RDWR);
		close(discovery_socket);
		discovery_socket = -1;
	}
	if (worker_started) {
		pthread_join(worker, NULL);
		worker_started = false;
	}
	stop_return_listener();
}
