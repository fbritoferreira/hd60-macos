// usbtrace: DYLD_INSERT_LIBRARIES shim that logs every IOKit USB call an app makes.
// Built x86_64 to inject into Elgato Game Capture HD running under Rosetta.
//
// It hooks IOCreatePlugInInterfaceForService, then swaps the vtable pointer of every
// IOUSBDevice/IOUSBInterface object handed out with a patched copy whose transfer
// functions log before/after calling the original.
//
// Output: $USBTRACE_DIR/trace.log (text) and $USBTRACE_DIR/pipe<N>.bin (raw bulk IN data).

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define INTERPOSE(repl, orig)                                                      \
	__attribute__((used)) static struct { const void *r, *o; } _interpose_##orig  \
	    __attribute__((section("__DATA,__interpose"))) = {(const void *)repl, (const void *)orig}

typedef IOUSBDeviceInterface942 Dev;
typedef IOUSBInterfaceInterface942 Intf;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static FILE *logf_;
static char dir[1024] = "/tmp/usbtrace";
static FILE *pipef[32];
static double t0;

static double now(void) {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

static void init(void) {
	if (logf_) return;
	const char *d = getenv("USBTRACE_DIR");
	if (d) snprintf(dir, sizeof dir, "%s", d);
	char p[1100];
	snprintf(p, sizeof p, "%s/trace.log", dir);
	logf_ = fopen(p, "w");
	setvbuf(logf_, NULL, _IOLBF, 0);
	t0 = now();
}

// Full hex up to `full` bytes, else head only.
static void hex(const void *buf, size_t n, size_t full) {
	const unsigned char *b = buf;
	size_t m = n <= full ? n : 64;
	for (size_t i = 0; i < m; i++) fprintf(logf_, "%02x", b[i]);
	if (m < n) fprintf(logf_, "...(+%zu)", n - m);
}

#define LOG(...)                                                        \
	do {                                                            \
		pthread_mutex_lock(&mu);                                \
		init();                                                 \
		fprintf(logf_, "%10.4f [%p] ", now() - t0, pthread_self()); \
		fprintf(logf_, __VA_ARGS__);                            \
		pthread_mutex_unlock(&mu);                              \
	} while (0)

#define LOGHEX(buf, n, full, ...)                                       \
	do {                                                            \
		pthread_mutex_lock(&mu);                                \
		init();                                                 \
		fprintf(logf_, "%10.4f [%p] ", now() - t0, pthread_self()); \
		fprintf(logf_, __VA_ARGS__);                            \
		hex(buf, n, full);                                      \
		fputc('\n', logf_);                                     \
		pthread_mutex_unlock(&mu);                              \
	} while (0)

static void dumppipe(int pipe, const void *buf, size_t n) {
	if (pipe < 0 || pipe >= 32 || !n) return;
	pthread_mutex_lock(&mu);
	if (!pipef[pipe]) {
		char p[1100];
		snprintf(p, sizeof p, "%s/pipe%d.bin", dir, pipe);
		pipef[pipe] = fopen(p, "w");
	}
	fwrite(buf, 1, n, pipef[pipe]);
	pthread_mutex_unlock(&mu);
}

// ponytail: per-pipe counter suppresses log spam from the video stream; raw data still goes to pipe<N>.bin
static unsigned long reads[32];
static int quiet(int pipe) {
	unsigned long c = pipe >= 0 && pipe < 32 ? reads[pipe]++ : 0;
	return c > 200 && c % 500;
}

// ---------------- device interface ----------------

static Dev origDev;

static void logreq(const char *fn, IOUSBDevRequest *r, IOReturn kr) {
	int in = (r->bmRequestType & 0x80) != 0;
	LOGHEX(r->pData, in ? r->wLenDone : r->wLength, 4096,
	       "%s bmRT=%02x bReq=%02x wValue=%04x wIndex=%04x wLength=%u done=%u kr=0x%x %s ", fn,
	       r->bmRequestType, r->bRequest, r->wValue, r->wIndex, r->wLength, r->wLenDone, kr,
	       in ? "IN" : "OUT");
}

static IOReturn hDeviceRequest(void *self, IOUSBDevRequest *r) {
	IOReturn kr = origDev.DeviceRequest(self, r);
	logreq("Dev.DeviceRequest", r, kr);
	return kr;
}

static IOReturn hDeviceRequestTO(void *self, IOUSBDevRequestTO *r) {
	IOReturn kr = origDev.DeviceRequestTO(self, r);
	IOUSBDevRequest q = {r->bmRequestType, r->bRequest, r->wValue, r->wIndex, r->wLength, r->pData, r->wLenDone};
	logreq("Dev.DeviceRequestTO", &q, kr);
	return kr;
}

static IOReturn hDeviceRequestAsync(void *self, IOUSBDevRequest *r, IOAsyncCallback1 cb, void *rc) {
	LOGHEX(r->pData, (r->bmRequestType & 0x80) ? 0 : r->wLength, 4096,
	       "Dev.DeviceRequestAsync bmRT=%02x bReq=%02x wValue=%04x wIndex=%04x wLength=%u ", r->bmRequestType,
	       r->bRequest, r->wValue, r->wIndex, r->wLength);
	return origDev.DeviceRequestAsync(self, r, cb, rc);
}

static IOReturn hDeviceRequestAsyncTO(void *self, IOUSBDevRequestTO *r, IOAsyncCallback1 cb, void *rc) {
	LOGHEX(r->pData, (r->bmRequestType & 0x80) ? 0 : r->wLength, 4096,
	       "Dev.DeviceRequestAsyncTO bmRT=%02x bReq=%02x wValue=%04x wIndex=%04x wLength=%u ", r->bmRequestType,
	       r->bRequest, r->wValue, r->wIndex, r->wLength);
	return origDev.DeviceRequestAsyncTO(self, r, cb, rc);
}

static IOReturn hUSBDeviceOpen(void *self) {
	IOReturn kr = origDev.USBDeviceOpen(self);
	LOG("Dev.USBDeviceOpen kr=0x%x\n", kr);
	return kr;
}

static IOReturn hUSBDeviceOpenSeize(void *self) {
	IOReturn kr = origDev.USBDeviceOpenSeize(self);
	LOG("Dev.USBDeviceOpenSeize kr=0x%x\n", kr);
	return kr;
}

static IOReturn hUSBDeviceClose(void *self) {
	LOG("Dev.USBDeviceClose\n");
	return origDev.USBDeviceClose(self);
}

static IOReturn hSetConfiguration(void *self, UInt8 v) {
	IOReturn kr = origDev.SetConfiguration(self, v);
	LOG("Dev.SetConfiguration %u kr=0x%x\n", v, kr);
	return kr;
}

static IOReturn hResetDevice(void *self) {
	LOG("Dev.ResetDevice\n");
	return origDev.ResetDevice(self);
}

static IOReturn hUSBDeviceReEnumerate(void *self, UInt32 opts) {
	LOG("Dev.USBDeviceReEnumerate 0x%x\n", opts);
	return origDev.USBDeviceReEnumerate(self, opts);
}

static Dev devV;
static void buildDev(void) {
	Dev v = origDev;
	v.DeviceRequest = hDeviceRequest;
	v.DeviceRequestTO = hDeviceRequestTO;
	v.DeviceRequestAsync = hDeviceRequestAsync;
	v.DeviceRequestAsyncTO = hDeviceRequestAsyncTO;
	v.USBDeviceOpen = hUSBDeviceOpen;
	v.USBDeviceOpenSeize = hUSBDeviceOpenSeize;
	v.USBDeviceClose = hUSBDeviceClose;
	v.SetConfiguration = hSetConfiguration;
	v.ResetDevice = hResetDevice;
	v.USBDeviceReEnumerate = hUSBDeviceReEnumerate;
	devV = v;
}

static Dev *patchedDev(Dev *orig) {
	static pthread_once_t once = PTHREAD_ONCE_INIT;
	if (orig == &devV) return orig;
	if (!origDev.DeviceRequest) memcpy(&origDev, orig, sizeof origDev);
	pthread_once(&once, buildDev);
	return &devV;
}

// ---------------- interface interface ----------------

static Intf origIntf;

typedef struct {
	IOAsyncCallback1 cb;
	void *refcon;
	void *buf;
	int pipe;
	int in;
} Ctx;

static void asyncDone(void *refcon, IOReturn result, void *arg0) {
	Ctx *c = refcon;
	UInt32 n = (UInt32)(uintptr_t)arg0;
	if (c->in) {
		dumppipe(c->pipe, c->buf, n);
		if (!quiet(c->pipe)) LOGHEX(c->buf, n, 512, "Intf.ReadPipeAsync done pipe=%d kr=0x%x n=%u ", c->pipe, result, n);
	} else {
		LOG("Intf.WritePipeAsync done pipe=%d kr=0x%x n=%u\n", c->pipe, result, n);
	}
	IOAsyncCallback1 cb = c->cb;
	void *rc = c->refcon;
	free(c);
	if (cb) cb(rc, result, arg0);
}

static Ctx *ctx(IOAsyncCallback1 cb, void *rc, void *buf, int pipe, int in) {
	Ctx *c = malloc(sizeof *c);
	*c = (Ctx){cb, rc, buf, pipe, in};
	return c;
}

static IOReturn hReadPipe(void *self, UInt8 pipe, void *buf, UInt32 *size) {
	UInt32 want = *size;
	IOReturn kr = origIntf.ReadPipe(self, pipe, buf, size);
	dumppipe(pipe, buf, *size);
	if (!quiet(pipe)) LOGHEX(buf, *size, 512, "Intf.ReadPipe pipe=%u want=%u got=%u kr=0x%x ", pipe, want, *size, kr);
	return kr;
}

static IOReturn hReadPipeTO(void *self, UInt8 pipe, void *buf, UInt32 *size, UInt32 d, UInt32 c) {
	UInt32 want = *size;
	IOReturn kr = origIntf.ReadPipeTO(self, pipe, buf, size, d, c);
	dumppipe(pipe, buf, *size);
	if (!quiet(pipe)) LOGHEX(buf, *size, 512, "Intf.ReadPipeTO pipe=%u want=%u got=%u kr=0x%x ", pipe, want, *size, kr);
	return kr;
}

static IOReturn hWritePipe(void *self, UInt8 pipe, void *buf, UInt32 size) {
	LOGHEX(buf, size, 4096, "Intf.WritePipe pipe=%u size=%u ", pipe, size);
	return origIntf.WritePipe(self, pipe, buf, size);
}

static IOReturn hWritePipeTO(void *self, UInt8 pipe, void *buf, UInt32 size, UInt32 d, UInt32 c) {
	LOGHEX(buf, size, 4096, "Intf.WritePipeTO pipe=%u size=%u ", pipe, size);
	return origIntf.WritePipeTO(self, pipe, buf, size, d, c);
}

static IOReturn hReadPipeAsync(void *self, UInt8 pipe, void *buf, UInt32 size, IOAsyncCallback1 cb, void *rc) {
	if (!quiet(pipe)) LOG("Intf.ReadPipeAsync pipe=%u size=%u\n", pipe, size);
	return origIntf.ReadPipeAsync(self, pipe, buf, size, asyncDone, ctx(cb, rc, buf, pipe, 1));
}

static IOReturn hReadPipeAsyncTO(void *self, UInt8 pipe, void *buf, UInt32 size, UInt32 d, UInt32 c,
				 IOAsyncCallback1 cb, void *rc) {
	if (!quiet(pipe)) LOG("Intf.ReadPipeAsyncTO pipe=%u size=%u\n", pipe, size);
	return origIntf.ReadPipeAsyncTO(self, pipe, buf, size, d, c, asyncDone, ctx(cb, rc, buf, pipe, 1));
}

static IOReturn hWritePipeAsync(void *self, UInt8 pipe, void *buf, UInt32 size, IOAsyncCallback1 cb, void *rc) {
	LOGHEX(buf, size, 4096, "Intf.WritePipeAsync pipe=%u size=%u ", pipe, size);
	return origIntf.WritePipeAsync(self, pipe, buf, size, asyncDone, ctx(cb, rc, buf, pipe, 0));
}

static IOReturn hWritePipeAsyncTO(void *self, UInt8 pipe, void *buf, UInt32 size, UInt32 d, UInt32 c,
				  IOAsyncCallback1 cb, void *rc) {
	LOGHEX(buf, size, 4096, "Intf.WritePipeAsyncTO pipe=%u size=%u ", pipe, size);
	return origIntf.WritePipeAsyncTO(self, pipe, buf, size, d, c, asyncDone, ctx(cb, rc, buf, pipe, 0));
}

static IOReturn hControlRequest(void *self, UInt8 pipe, IOUSBDevRequest *r) {
	IOReturn kr = origIntf.ControlRequest(self, pipe, r);
	logreq("Intf.ControlRequest", r, kr);
	return kr;
}

static IOReturn hControlRequestTO(void *self, UInt8 pipe, IOUSBDevRequestTO *r) {
	IOReturn kr = origIntf.ControlRequestTO(self, pipe, r);
	IOUSBDevRequest q = {r->bmRequestType, r->bRequest, r->wValue, r->wIndex, r->wLength, r->pData, r->wLenDone};
	logreq("Intf.ControlRequestTO", &q, kr);
	return kr;
}

static IOReturn hControlRequestAsync(void *self, UInt8 pipe, IOUSBDevRequest *r, IOAsyncCallback1 cb, void *rc) {
	LOGHEX(r->pData, (r->bmRequestType & 0x80) ? 0 : r->wLength, 4096,
	       "Intf.ControlRequestAsync bmRT=%02x bReq=%02x wValue=%04x wIndex=%04x wLength=%u ", r->bmRequestType,
	       r->bRequest, r->wValue, r->wIndex, r->wLength);
	return origIntf.ControlRequestAsync(self, pipe, r, cb, rc);
}

static IOReturn hControlRequestAsyncTO(void *self, UInt8 pipe, IOUSBDevRequestTO *r, IOAsyncCallback1 cb, void *rc) {
	LOGHEX(r->pData, (r->bmRequestType & 0x80) ? 0 : r->wLength, 4096,
	       "Intf.ControlRequestAsyncTO bmRT=%02x bReq=%02x wValue=%04x wIndex=%04x wLength=%u ", r->bmRequestType,
	       r->bRequest, r->wValue, r->wIndex, r->wLength);
	return origIntf.ControlRequestAsyncTO(self, pipe, r, cb, rc);
}

static IOReturn hUSBInterfaceOpen(void *self) {
	IOReturn kr = origIntf.USBInterfaceOpen(self);
	LOG("Intf.USBInterfaceOpen kr=0x%x\n", kr);
	return kr;
}

static IOReturn hSetAlternateInterface(void *self, UInt8 alt) {
	IOReturn kr = origIntf.SetAlternateInterface(self, alt);
	LOG("Intf.SetAlternateInterface %u kr=0x%x\n", alt, kr);
	return kr;
}

static IOReturn hClearPipeStall(void *self, UInt8 pipe) {
	LOG("Intf.ClearPipeStall pipe=%u\n", pipe);
	return origIntf.ClearPipeStall(self, pipe);
}

static IOReturn hClearPipeStallBothEnds(void *self, UInt8 pipe) {
	LOG("Intf.ClearPipeStallBothEnds pipe=%u\n", pipe);
	return origIntf.ClearPipeStallBothEnds(self, pipe);
}

static IOReturn hAbortPipe(void *self, UInt8 pipe) {
	LOG("Intf.AbortPipe pipe=%u\n", pipe);
	return origIntf.AbortPipe(self, pipe);
}

static IOReturn hResetPipe(void *self, UInt8 pipe) {
	LOG("Intf.ResetPipe pipe=%u\n", pipe);
	return origIntf.ResetPipe(self, pipe);
}

static IOReturn hGetPipeProperties(void *self, UInt8 pipe, UInt8 *dir_, UInt8 *num, UInt8 *type, UInt16 *mps,
				   UInt8 *interval) {
	IOReturn kr = origIntf.GetPipeProperties(self, pipe, dir_, num, type, mps, interval);
	LOG("Intf.GetPipeProperties pipe=%u dir=%u ep=%u type=%u mps=%u interval=%u\n", pipe, *dir_, *num, *type, *mps,
	    *interval);
	return kr;
}

static Intf intfV;
static void buildIntf(void) {
	Intf v = origIntf;
	v.ReadPipe = hReadPipe;
	v.ReadPipeTO = hReadPipeTO;
	v.WritePipe = hWritePipe;
	v.WritePipeTO = hWritePipeTO;
	v.ReadPipeAsync = hReadPipeAsync;
	v.ReadPipeAsyncTO = hReadPipeAsyncTO;
	v.WritePipeAsync = hWritePipeAsync;
	v.WritePipeAsyncTO = hWritePipeAsyncTO;
	v.ControlRequest = hControlRequest;
	v.ControlRequestTO = hControlRequestTO;
	v.ControlRequestAsync = hControlRequestAsync;
	v.ControlRequestAsyncTO = hControlRequestAsyncTO;
	v.USBInterfaceOpen = hUSBInterfaceOpen;
	v.SetAlternateInterface = hSetAlternateInterface;
	v.ClearPipeStall = hClearPipeStall;
	v.ClearPipeStallBothEnds = hClearPipeStallBothEnds;
	v.AbortPipe = hAbortPipe;
	v.ResetPipe = hResetPipe;
	v.GetPipeProperties = hGetPipeProperties;
	intfV = v;
}

static Intf *patchedIntf(Intf *orig) {
	static pthread_once_t once = PTHREAD_ONCE_INIT;
	if (orig == &intfV) return orig;
	if (!origIntf.ReadPipe) memcpy(&origIntf, orig, sizeof origIntf);
	pthread_once(&once, buildIntf);
	return &intfV;
}

// ---------------- plugin hook ----------------
// Device and interface plugins have different QueryInterface implementations, so each gets its own hook.

static IOCFPlugInInterface origDevPlug, origIntfPlug, devPlugV, intfPlugV;

static void uuidstr(REFIID iid, char *out, size_t n) {
	CFUUIDRef u = CFUUIDCreateFromUUIDBytes(NULL, iid);
	CFStringRef s = CFUUIDCreateString(NULL, u);
	CFStringGetCString(s, out, n, kCFStringEncodingUTF8);
	CFRelease(s);
	CFRelease(u);
}

static HRESULT qiDev(void *self, REFIID iid, LPVOID *out) {
	HRESULT hr = origDevPlug.QueryInterface(self, iid, out);
	char us[64];
	uuidstr(iid, us, sizeof us);
	LOG("Dev QueryInterface %s hr=0x%x -> %p\n", us, (int)hr, out ? *out : NULL);
	if (hr == S_OK && *out) *(void ***)*out = (void **)patchedDev(*(Dev **)*out);
	return hr;
}

static HRESULT qiIntf(void *self, REFIID iid, LPVOID *out) {
	HRESULT hr = origIntfPlug.QueryInterface(self, iid, out);
	char us[64];
	uuidstr(iid, us, sizeof us);
	LOG("Intf QueryInterface %s hr=0x%x -> %p\n", us, (int)hr, out ? *out : NULL);
	if (hr == S_OK && *out) *(void ***)*out = (void **)patchedIntf(*(Intf **)*out);
	return hr;
}

static kern_return_t myCreatePlugIn(io_service_t svc, CFUUIDRef type, CFUUIDRef iface, IOCFPlugInInterface ***pl,
				    SInt32 *score) {
	kern_return_t kr = IOCreatePlugInInterfaceForService(svc, type, iface, pl, score);
	int isDev = CFEqual(type, kIOUSBDeviceUserClientTypeID);
	int isIntf = CFEqual(type, kIOUSBInterfaceUserClientTypeID);
	io_name_t cls = "";
	IOObjectGetClass(svc, cls);
	LOG("IOCreatePlugInInterfaceForService class=%s dev=%d intf=%d kr=0x%x\n", cls, isDev, isIntf, kr);
	if (kr != KERN_SUCCESS) return kr;
	pthread_mutex_lock(&mu);
	if (isDev) {
		if (!origDevPlug.QueryInterface) {
			origDevPlug = ***pl;
			devPlugV = origDevPlug;
			devPlugV.QueryInterface = qiDev;
		}
		**pl = &devPlugV;
	} else if (isIntf) {
		if (!origIntfPlug.QueryInterface) {
			origIntfPlug = ***pl;
			intfPlugV = origIntfPlug;
			intfPlugV.QueryInterface = qiIntf;
		}
		**pl = &intfPlugV;
	}
	pthread_mutex_unlock(&mu);
	return kr;
}
INTERPOSE(myCreatePlugIn, IOCreatePlugInInterfaceForService);

__attribute__((constructor)) static void start(void) { LOG("usbtrace loaded pid=%d\n", getpid()); }
