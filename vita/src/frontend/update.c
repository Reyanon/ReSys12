// Update check and install (same as ReST-V vita/src/frontend/update.c). At start a thread asks the GitHub API for the
// releases of Reyanon/ReSys12; the releases newer than this build (ReSys12 publishes pre-releases, so they count) give
// the notes shown in the game list (Update / Close / Skip this version). An update downloads the release's package (the
// zip with the VPK, or a VPK), unpacks the VPK's files to ux0:data/resys12/update/files and exits; the kernel helper
// copies them into the app folder once the app is gone (no process can write the folder of a running app); the user
// starts it again.
#include "frontend/update.h"

#include <psp2/appmgr.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
#include <psp2/kernel/rng.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "junzip.h"
#include "platform/log.h"
#include "rs12_version.h"
#include "rs12_kctrl.h"
extern int rs12_kctrl_ready(void);

#define DATA_DIR "ux0:data/resys12"
#define UPD_DIR DATA_DIR "/update"
#define SKIP_PATH DATA_DIR "/update_skip.txt"
#define API_URL "https://api.github.com/repos/Reyanon/ReSys12/releases?per_page=30"
#define CA_PATH "app0:cacert.pem"

static volatile int s_ready;       // 1: a newer release is known
static char s_tag[32], s_url[512]; // its tag and package URL
static char *s_notes;              // notes of the newer releases
static char s_cur[32];             // this build's version (or ux0:data/resys12/updtest, a test)
static char s_api[256] = API_URL;  // (test: updtest "api=URL")
static char s_dl_test[256];        // (test: updtest "dl=URL", the package URL)
int rs12_update_auto;              // test: "updtest" = "vX.Y.Z auto" picks Update by itself

static void rmtree(const char *dir);

// ---- version "vA.B.C[-x]" as a number ----
static long long ver_num(const char *v)
{
	int a = 0, b = 0, c = 0;
	if (*v == 'v' || *v == 'V')
		v++;
	if (sscanf(v, "%d.%d.%d", &a, &b, &c) < 2)
		return -1;
	return ((long long)a << 40) | ((long long)b << 20) | c;
}

// ---- network (the UDP logger may have started it already) ----
static int s_net_mine, s_ctl_mine;
static int net_up(void)
{
	static char mem[256 * 1024];
	if (sceSysmoduleIsLoaded(SCE_SYSMODULE_NET) != SCE_SYSMODULE_LOADED)
		sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
	SceNetInitParam p;
	p.memory = mem;
	p.size = sizeof(mem);
	p.flags = 0;
	const int r = sceNetInit(&p);
	if (r < 0 && r != (int)SCE_NET_ERROR_EBUSY)
		return 0;
	s_net_mine = r >= 0;
	s_ctl_mine = sceNetCtlInit() >= 0;
	for (int i = 0; i < 100; i++) { // up to 10 s for a connection
		int st = 0;
		if (sceNetCtlInetGetState(&st) >= 0 && st == SCE_NETCTL_STATE_CONNECTED)
			return 1;
		sceKernelDelayThread(100 * 1000);
	}
	return 0;
}

// ---- HTTPS GET into memory (mbedTLS over the Vita's sockets: GPL-compatible libraries only, no OpenSSL) ----
typedef struct { char *p; size_t n, cap; void (*progress)(const char *, int); } Buf;
static int buf_add(Buf *b, const void *data, size_t k)
{
	if (b->n + k + 1 > b->cap) {
		size_t cap = b->cap ? b->cap * 2 : 65536;
		while (cap < b->n + k + 1)
			cap *= 2;
		char *q = (char *)realloc(b->p, cap);
		if (!q)
			return 0;
		b->p = q;
		b->cap = cap;
	}
	memcpy(b->p + b->n, data, k);
	b->n += k;
	b->p[b->n] = 0;
	return 1;
}
static int tls_rng(void *ctx, unsigned char *out, size_t len) // (mbedTLS random: the system's generator)
{
	(void)ctx;
	while (len) {
		const size_t k = len > 64 ? 64 : len;
		if (sceKernelGetRandomNumber(out, k) < 0)
			return -1;
		out += k;
		len -= k;
	}
	return 0;
}
// mbedtls_net_connect with a limit: the connect runs non-blocking and is asked again until it is done (with the
// router's internet down a connect waited minutes, the update window frozen); the socket then blocks again
static int connect_timed(mbedtls_net_context *net, const char *host, int seconds)
{
	struct addrinfo hints, *res = NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, "443", &hints, &res) != 0 || !res)
		return MBEDTLS_ERR_NET_UNKNOWN_HOST;
	int r = MBEDTLS_ERR_NET_CONNECT_FAILED;
	const int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (fd >= 0) {
		const int fl = fcntl(fd, F_GETFL, 0);
		fcntl(fd, F_SETFL, fl | O_NONBLOCK);
		int c = connect(fd, res->ai_addr, res->ai_addrlen);
		for (int i = 0; c != 0 && i < seconds * 20; i++) {
			if (errno == EISCONN) {
				c = 0;
				break;
			}
			if (errno != EINPROGRESS && errno != EALREADY && errno != EAGAIN)
				break;
			sceKernelDelayThread(50 * 1000);
			c = connect(fd, res->ai_addr, res->ai_addrlen);
		}
		fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
		if (c == 0) {
			net->fd = fd;
			r = 0;
		} else
			close(fd);
	}
	freeaddrinfo(res);
	return r;
}
static mbedtls_x509_crt s_ca;
static int s_ca_ok;
// GET url (https only, redirects followed): the body in b; 0 on failure
static int http_get(const char *url, Buf *b, long timeout_s)
{
	if (!s_ca_ok) {
		psa_crypto_init();
		mbedtls_x509_crt_init(&s_ca);
		const int r = mbedtls_x509_crt_parse_file(&s_ca, CA_PATH); // (> 0: some certificates left out)
		if (r < 0) {
			RS12_LOG("update: certificates %s: %x", CA_PATH, -r);
			return 0;
		}
		s_ca_ok = 1;
	}
	char cur[2048]; // (a release asset redirects to a long signed URL)
	snprintf(cur, sizeof(cur), "%s", url);
	for (int hop = 0; hop < 5; hop++) {
		char host[256], path[2048];
		if (strncmp(cur, "https://", 8))
			return 0;
		const char *h = cur + 8, *sl = strchr(h, '/');
		snprintf(host, sizeof(host), "%.*s", sl ? (int)(sl - h) : (int)strlen(h), h);
		snprintf(path, sizeof(path), "%s", sl ? sl : "/");
		mbedtls_net_context net;
		mbedtls_ssl_context ssl;
		mbedtls_ssl_config conf;
		mbedtls_net_init(&net);
		mbedtls_ssl_init(&ssl);
		mbedtls_ssl_config_init(&conf);
		Buf raw = { 0 };
		int ok = 0, e;
		if ((e = connect_timed(&net, host, 10)) != 0 ||
			(e = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)) != 0)
			RS12_LOG("update: connect %s: -%x", host, -e);
		else {
			mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
			mbedtls_ssl_conf_ca_chain(&conf, &s_ca, NULL);
			mbedtls_ssl_conf_rng(&conf, tls_rng, NULL);
			// the download stopped at 10% with mbedtls_net_recv_timeout: it waits in select(), which did not wake on
			// the device; a plain blocking read with the socket's own receive timeout instead
			struct timeval tv = { timeout_s < 30 ? timeout_s : 30, 0 };
			setsockopt(net.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
			if ((e = mbedtls_ssl_setup(&ssl, &conf)) == 0 && (e = mbedtls_ssl_set_hostname(&ssl, host)) == 0) {
				mbedtls_ssl_set_bio(&ssl, &net, mbedtls_net_send, mbedtls_net_recv, NULL);
				while ((e = mbedtls_ssl_handshake(&ssl)) == MBEDTLS_ERR_SSL_WANT_READ || e == MBEDTLS_ERR_SSL_WANT_WRITE)
					;
			}
			if (e != 0)
				RS12_LOG("update: TLS %s: -%x (verify %x)", host, -e, mbedtls_ssl_get_verify_result(&ssl));
			else {
				static char req[2560];
				const int rn = snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: ReSys12/" RS12_VERSION
					"\r\nAccept: application/vnd.github+json\r\nConnection: close\r\n\r\n", path, host);
				for (int w = 0; w < rn;) {
					e = mbedtls_ssl_write(&ssl, (const unsigned char *)req + w, (size_t)(rn - w));
					if (e > 0)
						w += e;
					else if (e != MBEDTLS_ERR_SSL_WANT_WRITE && e != MBEDTLS_ERR_SSL_WANT_READ)
						break;
				}
				static unsigned char rb[16384];
				long total = -1;
				size_t hdr = 0;
				int last = -2;
				for (;;) {
					e = mbedtls_ssl_read(&ssl, rb, sizeof(rb));
					if (e == MBEDTLS_ERR_SSL_WANT_READ || e == MBEDTLS_ERR_SSL_WANT_WRITE)
						continue;
					if (e <= 0) // (0 / peer close: done; a timeout or an error: what came is checked below)
						break;
					if (!buf_add(&raw, rb, (size_t)e))
						break;
					if (!hdr) {
						char *eh = strstr(raw.p, "\r\n\r\n");
						if (eh) {
							hdr = (size_t)(eh - raw.p) + 4;
							const char *cl = strstr(raw.p, "\r\nContent-Length:");
							if (!cl) cl = strstr(raw.p, "\r\ncontent-length:");
							if (cl && cl < eh)
								total = atol(cl + 17);
						}
					}
					if (hdr && total > 0 && b->progress) {
						const int pct = (int)((long long)(raw.n - hdr) * 100 / total);
						if (pct != last) {
							last = pct;
							b->progress("Downloading", pct);
						}
					}
					if (hdr && total >= 0 && raw.n - hdr >= (size_t)total)
						break;
				}
				RS12_LOG("update: %s: %u bytes (body %ld), last read %d", host, (unsigned)raw.n, total, e);
				ok = hdr > 0 && (total < 0 || raw.n - hdr >= (size_t)total);
			}
			mbedtls_ssl_close_notify(&ssl);
		}
		mbedtls_ssl_free(&ssl);
		mbedtls_ssl_config_free(&conf);
		mbedtls_net_free(&net);
		if (!ok) {
			free(raw.p);
			return 0;
		}
		int code = 0;
		sscanf(raw.p, "HTTP/%*s %d", &code);
		char *eh = strstr(raw.p, "\r\n\r\n");
		*eh = 0;
		if (code >= 300 && code < 400) { // redirect (the release asset goes to its storage host)
			const char *loc = strstr(raw.p, "\r\nLocation: ");
			if (!loc) loc = strstr(raw.p, "\r\nlocation: ");
			if (!loc) {
				free(raw.p);
				return 0;
			}
			loc += 12;
			snprintf(cur, sizeof(cur), "%.*s", (int)strcspn(loc, "\r\n"), loc);
			free(raw.p);
			continue;
		}
		if (code != 200) {
			RS12_LOG("update: GET %s: http %d", cur, code);
			free(raw.p);
			return 0;
		}
		const int chunked = strstr(raw.p, "chunked") != NULL;
		const char *body = eh + 4, *end = raw.p + raw.n;
		if (!chunked)
			ok = buf_add(b, body, (size_t)(end - body));
		else { // Transfer-Encoding: chunked
			ok = 1;
			while (ok && body < end) {
				char *x;
				const unsigned long k = strtoul(body, &x, 16);
				const char *d = strstr(x, "\r\n");
				if (!d || !k)
					break;
				d += 2;
				if (d + k > end) {
					ok = 0;
					break;
				}
				ok = buf_add(b, d, k);
				body = d + k + 2;
			}
		}
		free(raw.p);
		return ok;
	}
	return 0;
}

// ---- the little JSON the release list needs ----
// the string value of "key" inside [p, e) (unescaped into out); 0 if absent
static int json_str(const char *p, const char *e, const char *key, char *out, size_t outsz)
{
	char k[48];
	snprintf(k, sizeof(k), "\"%s\"", key);
	const size_t kl = strlen(k);
	for (const char *q = p; q + kl < e; q++) {
		if (memcmp(q, k, kl))
			continue;
		const char *v = q + kl;
		while (v < e && (*v == ' ' || *v == ':' || *v == '\t' || *v == '\n' || *v == '\r'))
			v++;
		if (v >= e || *v != '"')
			return 0;
		v++;
		size_t n = 0;
		while (v < e && *v != '"') {
			unsigned ch = (unsigned char)*v++;
			if (ch == '\\' && v < e) {
				const char c = *v++;
				ch = c == 'n' ? '\n' : c == 't' ? ' ' : c == 'r' ? '\r' : c == 'b' || c == 'f' ? ' ' : (unsigned char)c;
				if (c == 'u' && v + 4 <= e) {
					char hx[5] = { v[0], v[1], v[2], v[3], 0 };
					const unsigned u = (unsigned)strtoul(hx, NULL, 16);
					v += 4;
					// (UTF-8; the PGF font shows Latin text)
					if (u < 0x80)
						ch = u;
					else {
						char tmp[3];
						int m = 0;
						if (u < 0x800) {
							tmp[m++] = (char)(0xC0 | (u >> 6));
						} else {
							tmp[m++] = (char)(0xE0 | (u >> 12));
							tmp[m++] = (char)(0x80 | ((u >> 6) & 0x3F));
						}
						for (int i = 0; i < m && n + 1 < outsz; i++)
							out[n++] = tmp[i];
						ch = 0x80 | (u & 0x3F);
					}
				}
			}
			if (ch != '\r' && n + 1 < outsz)
				out[n++] = (char)ch;
		}
		out[n] = 0;
		return 1;
	}
	return 0;
}
// the end of the JSON object or array starting at p (strings skipped)
static const char *json_end(const char *p, const char *e)
{
	int depth = 0, str = 0;
	for (; p < e; p++) {
		if (str) {
			if (*p == '\\')
				p++;
			else if (*p == '"')
				str = 0;
		} else if (*p == '"')
			str = 1;
		else if (*p == '{' || *p == '[')
			depth++;
		else if (*p == '}' || *p == ']') {
			if (--depth == 0)
				return p + 1;
		}
	}
	return e;
}

// markdown lines to plain text: headings without '#', no '**' / '`'
static void plain_notes(char *s)
{
	char *w = s;
	int bol = 1;
	for (char *r = s; *r;) {
		if (bol && *r == '|') { // (markdown tables: left out)
			while (*r && *r != '\n')
				r++;
			if (*r)
				r++;
			continue;
		}
		if (bol) {
			while (*r == '#')
				r++;
			if (r > s && r[-1] == '#' && *r == ' ')
				r++;
		}
		if (r[0] == '*' && r[1] == '*') {
			r += 2;
			continue;
		}
		if (*r == '`') {
			r++;
			continue;
		}
		bol = *r == '\n';
		*w++ = *r++;
	}
	*w = 0;
}

static int check_thread(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	if (!net_up()) {
		RS12_LOG("update: no network");
		return sceKernelExitDeleteThread(0);
	}
	Buf b = { 0 };
	if (!http_get(s_api, &b, 20)) {
		free(b.p);
		return sceKernelExitDeleteThread(0);
	}
	char skip[32] = { 0 };
	FILE *sf = fopen(SKIP_PATH, "r");
	if (sf) {
		if (fscanf(sf, "%31s", skip) != 1)
			skip[0] = 0;
		fclose(sf);
	}
	const long long cur = ver_num(s_cur);
	long long best = cur;
	size_t notes_n = 0, notes_cap = 64 * 1024;
	char *notes = (char *)malloc(notes_cap);
	static char body[24 * 1024];
	const char *e = b.p + b.n, *p = b.p;
	while (p < e && *p != '[')
		p++;
	for (p++; p < e; p++) {
		if (*p != '{')
			continue;
		const char *oe = json_end(p, e);
		char tag[32], url[512], date[32];
		const char *dr = strstr(p, "\"draft\"");
		const int draft = dr && dr < oe && strstr(dr, "true") && strstr(dr, "true") < dr + 15;
		if (json_str(p, oe, "tag_name", tag, sizeof(tag)) && !draft && ver_num(tag) > cur) {
			// the package: a "_vita.zip" (the VPK inside) or a .vpk
			url[0] = 0;
			for (const char *q = p; q < oe; q++) {
				const char *k = strstr(q, "\"browser_download_url\"");
				if (!k || k >= oe)
					break;
				char u[512];
				if (json_str(k, oe, "browser_download_url", u, sizeof(u))) {
					const size_t ul = strlen(u);
					if ((ul > 9 && !strcmp(u + ul - 9, "_vita.zip")) || (ul > 4 && !strcmp(u + ul - 4, ".vpk")))
						snprintf(url, sizeof(url), "%s", u);
				}
				q = k + 1;
			}
			if (url[0]) {
				if (ver_num(tag) > best) {
					best = ver_num(tag);
					snprintf(s_tag, sizeof(s_tag), "%s", tag);
					snprintf(s_url, sizeof(s_url), "%s", url);
				}
				if (!json_str(p, oe, "published_at", date, sizeof(date)))
					date[0] = 0;
				date[10] = 0;
				if (!json_str(p, oe, "body", body, sizeof(body)))
					body[0] = 0;
				plain_notes(body);
				if (notes && notes_n + strlen(body) + 64 < notes_cap)
					notes_n += (size_t)snprintf(notes + notes_n, notes_cap - notes_n, "#%s  (%s)\n%s\n\n", tag, date, body);
			}
		}
		p = oe - 1;
	}
	free(b.p);
	if (best > cur && s_dl_test[0])
		snprintf(s_url, sizeof(s_url), "%s", s_dl_test);
	if (best > cur && strcmp(skip, s_tag) != 0) {
		s_notes = notes;
		RS12_LOG("update: %s available (this build %s), %s", s_tag, s_cur, s_url);
		__sync_synchronize();
		s_ready = 1;
	} else {
		free(notes);
		RS12_LOG("update: none (this build %s%s%s)", s_cur, best > cur ? ", skipped " : "", best > cur ? s_tag : "");
	}
	return sceKernelExitDeleteThread(0);
}

void rs12_update_check_start(void)
{
	SceIoStat st;
	FILE *rf = fopen(DATA_DIR "/update_result.txt", "r"); // the last install (kernel helper): files, failed, launch
	if (rf) {
		int n = 0, bad = 0, l = 0;
		if (fscanf(rf, "%d %d %d", &n, &bad, &l) == 3)
			RS12_LOG("update: last install %d files, %d failed, launch %08x", n, bad, (unsigned)l);
		fclose(rf);
		sceIoRemove(DATA_DIR "/update_result.txt");
	}
	if (sceIoGetstat(UPD_DIR, &st) >= 0)
		rmtree(UPD_DIR); // (an install done or given up)
	const int test = sceIoGetstat(DATA_DIR "/updtest", &st) >= 0;
	if ((!test && (sceIoGetstat(DATA_DIR "/autostart", &st) >= 0 || sceIoGetstat(DATA_DIR "/restart", &st) >= 0)) ||
		sceIoGetstat(DATA_DIR "/noupdate", &st) >= 0)
		return; // (measuring tools, except a test; ux0:data/resys12/noupdate turns the check off)
	snprintf(s_cur, sizeof(s_cur), "%s", RS12_VERSION);
	FILE *tf = fopen(DATA_DIR "/updtest", "r"); // test: pretend to be this version
	if (tf) {
		// "vX.Y.Z [auto] [api=URL] [dl=URL]": unreachable URLs test the failures (nothing may crash offline)
		if (fscanf(tf, "%31s", s_cur) != 1)
			snprintf(s_cur, sizeof(s_cur), "%s", RS12_VERSION);
		char tok[256];
		while (fscanf(tf, "%255s", tok) == 1) {
			if (!strcmp(tok, "auto"))
				rs12_update_auto = 1;
			else if (!strncmp(tok, "api=", 4))
				snprintf(s_api, sizeof(s_api), "%s", tok + 4);
			else if (!strncmp(tok, "dl=", 3))
				snprintf(s_dl_test, sizeof(s_dl_test), "%s", tok + 3);
		}
		fclose(tf);
	}
	extern int rs12_cpu3_mask(void);
	const SceUID t = sceKernelCreateThread("rs12_update", check_thread, 0x10000100 + 30, 0x10000, 0, rs12_cpu3_mask(), NULL);
	if (t >= 0)
		sceKernelStartThread(t, 0, NULL);
}

int rs12_update_ready(void) { return s_ready; }
const char *rs12_update_version(void) { return s_tag; }
const char *rs12_update_notes(void) { return s_notes ? s_notes : ""; }

void rs12_update_skip(void)
{
	FILE *f = fopen(SKIP_PATH, "w");
	if (f) {
		fprintf(f, "%s\n", s_tag);
		fclose(f);
	}
	s_ready = 0;
}

// ---- install ----
static void mkdirs(const char *path) // every directory of a file path
{
	char tmp[512];
	snprintf(tmp, sizeof(tmp), "%s", path);
	for (char *q = tmp + 5; *q; q++)
		if (*q == '/') {
			*q = 0;
			sceIoMkdir(tmp, 0777);
			*q = '/';
		}
}
static void rmtree(const char *dir)
{
	const SceUID d = sceIoDopen(dir);
	if (d >= 0) {
		SceIoDirent ent;
		while (sceIoDread(d, &ent) > 0) {
			if (!strcmp(ent.d_name, ".") || !strcmp(ent.d_name, ".."))
				continue;
			char p[512];
			snprintf(p, sizeof(p), "%s/%s", dir, ent.d_name);
			if (SCE_S_ISDIR(ent.d_stat.st_mode))
				rmtree(p);
			else
				sceIoRemove(p);
		}
		sceIoDclose(d);
	}
	sceIoRmdir(dir);
}

typedef struct { JZFileHeader h[256]; char name[256][128]; int n; } ZipList;
static int zip_rec(JZFile *zip, int index, JZFileHeader *h, char *name, void *user)
{
	(void)zip;
	(void)index;
	ZipList *l = (ZipList *)user;
	if (l->n >= 256)
		return 0;
	l->h[l->n] = *h;
	snprintf(l->name[l->n], sizeof(l->name[0]), "%s", name);
	l->n++;
	return 1;
}
// every entry of the zip at path: to dir (files), or only the first ending in want (to want_out); 1 if done
static int unzip(const char *path, const char *dir, const char *want, const char *want_out)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return 0;
	JZFile *z = jzfile_from_stdio_file(fp);
	JZEndRecord er;
	static ZipList l;
	l.n = 0;
	int ok = jzReadEndRecord(z, &er) == 0 && jzReadCentralDirectory(z, &er, zip_rec, &l) == 0;
	int found = 0;
	for (int i = 0; ok && i < l.n; i++) {
		const char *nm = l.name[i];
		const size_t nl = strlen(nm);
		if (!nl || nm[nl - 1] == '/')
			continue;
		if (want && (nl < strlen(want) || strcmp(nm + nl - strlen(want), want)))
			continue;
		if (strstr(nm, "..") || nm[0] == '/')
			continue;
		JZFileHeader h;
		char lname[256];
		if (z->seek(z, l.h[i].offset, SEEK_SET) || jzReadLocalFileHeader(z, &h, lname, sizeof(lname))) {
			ok = 0;
			break;
		}
		void *data = malloc(h.uncompressedSize ? h.uncompressedSize : 1);
		if (!data || jzReadData(z, &h, data) != 0) {
			free(data);
			ok = 0;
			break;
		}
		char out[512];
		if (want)
			snprintf(out, sizeof(out), "%s", want_out);
		else
			snprintf(out, sizeof(out), "%s/%s", dir, nm);
		mkdirs(out);
		FILE *of = fopen(out, "wb");
		ok = of && fwrite(data, 1, h.uncompressedSize, of) == h.uncompressedSize;
		if (of)
			fclose(of);
		free(data);
		found++;
		if (want)
			break;
	}
	z->close(z);
	return ok && found > 0;
}

const char *rs12_update_install(void (*progress)(const char *stage, int percent))
{
	SceIoStat st;
	if (!rs12_kctrl_ready())
		return "The kernel helper is not loaded: install the VPK by hand";
	rmtree(UPD_DIR);
	sceIoMkdir(UPD_DIR, 0777);
	progress("Downloading", 0);
	Buf b = { 0 };
	b.progress = progress;
	if (!http_get(s_url, &b, 600)) {
		free(b.p);
		return "Download failed";
	}
	progress("Unpacking", -1);
	const size_t ul = strlen(s_url);
	const int is_vpk = ul > 4 && !strcmp(s_url + ul - 4, ".vpk");
	const char *pkg = is_vpk ? UPD_DIR "/pkg.vpk" : UPD_DIR "/pkg.zip";
	FILE *f = fopen(pkg, "wb");
	const int wrote = f && fwrite(b.p, 1, b.n, f) == b.n;
	if (f)
		fclose(f);
	free(b.p);
	if (!wrote)
		return "Could not write the package";
	if (!is_vpk && !unzip(UPD_DIR "/pkg.zip", NULL, ".vpk", UPD_DIR "/pkg.vpk"))
		return "No VPK in the package";
	if (!unzip(UPD_DIR "/pkg.vpk", UPD_DIR "/files", NULL, NULL) || sceIoGetstat(UPD_DIR "/files/eboot.bin", &st) < 0)
		return "Could not unpack the VPK";
	sceIoRemove(UPD_DIR "/pkg.zip");
	sceIoRemove(UPD_DIR "/pkg.vpk");
	FILE *m = fopen(UPD_DIR "/version.txt", "w");
	if (m) {
		fprintf(m, "%s\n", s_tag);
		fclose(m);
	}
	RS12_LOG("update: %s unpacked, the kernel helper installs it after exit", s_tag);
	rs12KCtrlInstallOnExit();
	for (int i = 5; i > 0; i--) { // (percent -2: the stage text alone)
		char t[96];
		snprintf(t, sizeof(t), "ReSys12 closes in %d s to install %s. Start it again then.", i, s_tag);
		progress(t, -2);
		sceKernelDelayThread(1000 * 1000);
	}
	sceKernelExitProcess(0);
	return "Could not exit";
}
