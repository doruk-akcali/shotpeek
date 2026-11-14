// ShotPeek: macOS-style screenshot preview for Windows.
//
// F6 freezes the screen instantly (ShotPeek is already running, so there is no
// app to start): drag a box to capture it, Esc or right-click cancels. The shot
// is saved to the Screenshots folder and copied to the clipboard.
// Every new image in the Screenshots folder (ShotPeek's own, and Win+Shift+S
// snips from Snipping Tool) slides in as a thumbnail at the bottom-right of the
// screen for a few seconds:
//   drag it      -> drops the image file into any app (chat, mail, folder)
//   click it     -> opens the image
//   swipe right  -> dismisses it
//   right-click  -> Open / Show in folder / Copy / Close / Quit
//
// Build (w64devkit):
//   gcc -std=c99 -Os -municode -mwindows -s -o shotpeek.exe src/shotpeek.c -lgdiplus -lole32 -luuid -lshell32 -lshcore -ldwmapi -lwinmm -lgdi32

#define COBJMACROS
#include <windows.h>
#include <windowsx.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <dwmapi.h>
#include <mmsystem.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

//================
// GDI+ flat API (declared here; only these few calls are needed)
//================

typedef struct { UINT32 GdiplusVersion; void *DebugEventCallback; BOOL SuppressBackgroundThread; BOOL SuppressExternalCodecs; } GpStartupInput;
int WINAPI GdiplusStartup(ULONG_PTR *token, const GpStartupInput *input, void *output);
int WINAPI GdipCreateBitmapFromFile(const WCHAR *file, void **bitmap);
int WINAPI GdipCreateBitmapFromScan0(INT width, INT height, INT stride, INT format, BYTE *scan0, void **bitmap);
int WINAPI GdipGetImageWidth(void *image, UINT *width);
int WINAPI GdipGetImageHeight(void *image, UINT *height);
int WINAPI GdipGetImageGraphicsContext(void *image, void **graphics);
int WINAPI GdipSetInterpolationMode(void *graphics, INT mode);
int WINAPI GdipSetPixelOffsetMode(void *graphics, INT mode);
int WINAPI GdipDrawImageRectI(void *graphics, void *image, INT x, INT y, INT width, INT height);
int WINAPI GdipDeleteGraphics(void *graphics);
int WINAPI GdipDisposeImage(void *image);
int WINAPI GdipSaveImageToFile(void *image, const WCHAR *file, const CLSID *encoder, const void *params);
#define PixelFormat32bppPARGB      0x000E200B
#define PixelFormat32bppRGB        0x00022009
static const CLSID PngEncoder = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
#define InterpolationHighQualityBicubic 7
#define PixelOffsetHighQuality     2

//================
// State
//================

#define WM_NEWSHOT  (WM_APP + 1) // lparam: malloc'd path from the watcher thread
#define WM_ANIMDONE (WM_APP + 2) // wparam: animation generation, lparam: hide afterwards
#define TIMER_DISMISS 1
#define TIMER_RETRY   2
#define SHOW_MS    6000
#define HOTKEY_F6  1

static HWND          Peek;            // The thumbnail window (layered, never activates)
static wchar_t       ShotDir[MAX_PATH];
static wchar_t       ShotPath[MAX_PATH];  // Image shown in the thumbnail
static wchar_t       PendingPath[MAX_PATH]; // New image still being written
static int           PendingTries;
static float         Scale = 1;
static int           PeekW, PeekH;   // Window size, shadow included
static POINT         PeekHome;       // Resting position
static int           PeekGone;       // x where the window is fully off the right edge
static int           PeekX;          // Current x
static bool          Hovering, Pressed, Swiping;
static POINT         PressAt;
static volatile LONG AnimGen;

static void Place(int x)
{
	PeekX = x;
	POINT pos = {x, PeekHome.y};
	BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
	UpdateLayeredWindow(Peek, NULL, &pos, NULL, NULL, NULL, 0, &blend, ULW_ALPHA); // Moves without re-sending the bitmap
}

//================
// Slide animation, synced to the display
//================

struct anim { LONG gen; int x0, x1; DWORD ms; bool hide; };

static DWORD WINAPI AnimThread(LPVOID param)
{
	struct anim a = *(struct anim *)param;
	free(param);
	DWORD start = timeGetTime();
	for (;;) {
		if (AnimGen != a.gen) return 0; // Superseded
		float t = (timeGetTime() - start) / (float)a.ms;
		if (t > 1) t = 1;
		float e = 1 - (1 - t) * (1 - t) * (1 - t); // Ease-out cubic
		Place(a.x0 + (int)((a.x1 - a.x0) * e));
		if (t >= 1) break;
		DwmFlush();
	}
	PostMessageW(Peek, WM_ANIMDONE, (WPARAM)a.gen, a.hide);
	return 0;
}

static void Animate(int to, DWORD ms, bool hide)
{
	struct anim *a = malloc(sizeof *a);
	if (!a) return;
	*a = (struct anim){InterlockedIncrement(&AnimGen), PeekX, to, ms, hide};
	HANDLE thread = CreateThread(NULL, 0, AnimThread, a, 0, NULL);
	if (thread) CloseHandle(thread); else free(a);
}

static void StopAnimation(void) { InterlockedIncrement(&AnimGen); }

static void Dismiss(void)
{
	KillTimer(Peek, TIMER_DISMISS);
	Animate(PeekGone, 400, true);
}

static void HideNow(void)
{
	KillTimer(Peek, TIMER_DISMISS);
	StopAnimation();
	ShowWindow(Peek, SW_HIDE);
}

//================
// Thumbnail drawing
//================

static float RoundRectDistance(float x, float y, float cx, float cy, float hw, float hh, float r) // Signed: < 0 inside
{
	float dx = fabsf(x - cx) - (hw - r), dy = fabsf(y - cy) - (hh - r);
	float ox = dx > 0 ? dx : 0, oy = dy > 0 ? dy : 0;
	float inside = dx > dy ? dx : dy;
	return sqrtf(ox * ox + oy * oy) + (inside < 0 ? inside : 0) - r;
}

static bool RenderPeek(const wchar_t *path)
{
	void *image = NULL;
	if (GdipCreateBitmapFromFile(path, &image) != 0 || !image) return false;
	UINT iw = 0, ih = 0;
	GdipGetImageWidth(image, &iw);
	GdipGetImageHeight(image, &ih);
	if (!iw || !ih) { GdipDisposeImage(image); return false; }

	// Monitor under the mouse, bottom-right corner
	POINT mouse; GetCursorPos(&mouse);
	HMONITOR monitor = MonitorFromPoint(mouse, MONITOR_DEFAULTTOPRIMARY);
	MONITORINFO mi = {.cbSize = sizeof mi};
	GetMonitorInfoW(monitor, &mi);
	UINT dpiX = 96, dpiY = 96;
	GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
	Scale = dpiX / 96.0f;

	float fit = fminf(300 * Scale / iw, 200 * Scale / ih);
	if (fit > 1) fit = 1;
	int tw = (int)(iw * fit + 0.5f), th = (int)(ih * fit + 0.5f);
	if (tw < 24 * Scale) tw = 24 * Scale;
	if (th < 24 * Scale) th = 24 * Scale;
	int pad = 16 * Scale; // Room for the shadow
	int W = tw + 2 * pad, H = th + 2 * pad;

	// Scaled image, high quality, flattened onto white (transparent screenshots stay readable)
	uint32_t *thumb = calloc((size_t)tw * th, 4);
	if (!thumb) { GdipDisposeImage(image); return false; }
	void *target = NULL, *graphics = NULL;
	GdipCreateBitmapFromScan0(tw, th, tw * 4, PixelFormat32bppPARGB, (BYTE *)thumb, &target);
	if (target && GdipGetImageGraphicsContext(target, &graphics) == 0) {
		GdipSetInterpolationMode(graphics, InterpolationHighQualityBicubic);
		GdipSetPixelOffsetMode(graphics, PixelOffsetHighQuality);
		GdipDrawImageRectI(graphics, image, 0, 0, tw, th);
		GdipDeleteGraphics(graphics);
	}
	if (target) GdipDisposeImage(target);
	GdipDisposeImage(image); // Releases the file

	HDC screen = GetDC(NULL);
	BITMAPINFO bmi = {0};
	bmi.bmiHeader.biSize = sizeof bmi.bmiHeader; bmi.bmiHeader.biWidth = W; bmi.bmiHeader.biHeight = -H;
	bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32; bmi.bmiHeader.biCompression = BI_RGB;
	uint32_t *px = NULL;
	HBITMAP bitmap = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
	if (!bitmap) { free(thumb); ReleaseDC(NULL, screen); return false; }

	// Rounded image with a thin light rim, over a soft drop shadow (premultiplied BGRA)
	float radius = 9 * Scale, rim = Scale < 1 ? 1 : Scale, blur = 7 * Scale, drop = 3 * Scale;
	float cx = W / 2.0f, cy = H / 2.0f, hw = tw / 2.0f, hh = th / 2.0f;
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			float sd = RoundRectDistance(x + 0.5f, y + 0.5f, cx, cy, hw, hh, radius);
			float cover = 0.5f - sd; cover = cover < 0 ? 0 : cover > 1 ? 1 : cover;
			float shadowD = RoundRectDistance(x + 0.5f, y + 0.5f, cx, cy + drop, hw, hh, radius);
			float shadow = shadowD <= 0 ? 0.42f : 0.42f * expf(-(shadowD * shadowD) / (blur * blur));
			float r = 0, g = 0, b = 0;
			if (cover > 0) {
				int sx = x - pad, sy = y - pad;
				sx = sx < 0 ? 0 : sx >= tw ? tw - 1 : sx;
				sy = sy < 0 ? 0 : sy >= th ? th - 1 : sy;
				uint32_t p = thumb[(size_t)sy * tw + sx];
				float a = (p >> 24) / 255.0f;
				r = (p >> 16 & 0xFF) + 255 * (1 - a);
				g = (p >> 8 & 0xFF) + 255 * (1 - a);
				b = (p & 0xFF) + 255 * (1 - a);
				if (sd > -rim) { r = r * 0.45f + 255 * 0.55f; g = g * 0.45f + 255 * 0.55f; b = b * 0.45f + 255 * 0.55f; }
			}
			float alpha = cover + shadow * (1 - cover);
			px[(size_t)y * W + x] = (uint32_t)(alpha * 255 + 0.5f) << 24 | (uint32_t)(r * cover + 0.5f) << 16 | (uint32_t)(g * cover + 0.5f) << 8 | (uint32_t)(b * cover + 0.5f);
		}
	}
	free(thumb);

	int margin = 14 * Scale;
	PeekW = W; PeekH = H;
	PeekHome.x = mi.rcWork.right - W - margin + pad;
	PeekHome.y = mi.rcWork.bottom - H - 60 * Scale + pad; // Clear of the auto-hidden taskbar when it pops up
	PeekGone = mi.rcMonitor.right + 2;

	HDC memory = CreateCompatibleDC(screen);
	HGDIOBJ old = SelectObject(memory, bitmap);
	POINT pos = {Pressed || Swiping ? PeekX : PeekGone, PeekHome.y}, zero = {0, 0};
	SIZE size = {W, H};
	BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
	StopAnimation();
	UpdateLayeredWindow(Peek, screen, &pos, &size, memory, &zero, 0, &blend, ULW_ALPHA);
	PeekX = pos.x;
	SelectObject(memory, old);
	DeleteDC(memory);
	DeleteObject(bitmap);
	ReleaseDC(NULL, screen);
	return true;
}

static void ShowShot(const wchar_t *path)
{
	Pressed = Swiping = false;
	if (!RenderPeek(path)) return;
	wcsncpy(ShotPath, path, MAX_PATH - 1);
	SetWindowPos(Peek, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
	KillTimer(Peek, TIMER_DISMISS);
	Animate(PeekHome.x, 300, false); // The 6 s start once it has arrived (WM_ANIMDONE)
}

//================
// Drag, open, copy
//================

static const GUID BHID_DataObjectLocal = {0xb8c0bd9f, 0xed24, 0x455c, {0x83, 0xe6, 0xd5, 0x39, 0x0c, 0x4f, 0xe8, 0xc4}};

static IDataObject *ShotDataObject(void) // The file as Explorer offers it, so every app accepts the drop
{
	IShellItem *item = NULL;
	IDataObject *data = NULL;
	if (SUCCEEDED(SHCreateItemFromParsingName(ShotPath, NULL, &IID_IShellItem, (void **)&item))) {
		IShellItem_BindToHandler(item, NULL, &BHID_DataObjectLocal, &IID_IDataObject, (void **)&data);
		IShellItem_Release(item);
	}
	return data;
}

static void DragShot(void)
{
	IDataObject *data = ShotDataObject();
	if (!data) return;
	KillTimer(Peek, TIMER_DISMISS);
	StopAnimation();
	ShowWindow(Peek, SW_HIDE); // The shell's drag image (the thumbnail) follows the cursor instead
	DWORD effect = DROPEFFECT_NONE;
	HRESULT result = SHDoDragDrop(Peek, data, NULL, DROPEFFECT_COPY, &effect);
	IDataObject_Release(data);
	if (result == DRAGDROP_S_DROP && effect != DROPEFFECT_NONE) return; // Dropped: done, like macOS
	// Cancelled: come back
	Place(PeekHome.x);
	ShowWindow(Peek, SW_SHOWNOACTIVATE);
	SetTimer(Peek, TIMER_DISMISS, SHOW_MS, NULL);
}

static void OpenShot(void)  { ShellExecuteW(NULL, L"open", ShotPath, NULL, NULL, SW_SHOWNORMAL); }

static void RevealShot(void)
{
	PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(ShotPath);
	if (pidl) { SHOpenFolderAndSelectItems(pidl, 0, NULL, 0); ILFree(pidl); }
}

static void CopyShot(void)
{
	IDataObject *data = ShotDataObject();
	if (data) { OleSetClipboard(data); OleFlushClipboard(); IDataObject_Release(data); }
}

static void ShowMenu(void)
{
	KillTimer(Peek, TIMER_DISMISS);
	HMENU menu = CreatePopupMenu();
	AppendMenuW(menu, MF_STRING, 1, L"Open");
	AppendMenuW(menu, MF_STRING, 2, L"Show in folder");
	AppendMenuW(menu, MF_STRING, 3, L"Copy");
	AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
	AppendMenuW(menu, MF_STRING, 4, L"Close");
	AppendMenuW(menu, MF_STRING, 5, L"Quit ShotPeek");
	POINT at; GetCursorPos(&at);
	SetForegroundWindow(Peek); // Lets the menu close when clicking elsewhere
	int choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, at.x, at.y, 0, Peek, NULL);
	DestroyMenu(menu);
	switch (choice) {
		case 1: OpenShot(); HideNow(); break;
		case 2: RevealShot(); HideNow(); break;
		case 3: CopyShot(); Dismiss(); break;
		case 4: Dismiss(); break;
		case 5: DestroyWindow(Peek); break;
		default: SetTimer(Peek, TIMER_DISMISS, SHOW_MS, NULL);
	}
}

//================
// Screenshots folder watcher
//================

static bool IsImageName(const wchar_t *name)
{
	const wchar_t *ext = wcsrchr(name, L'.');
	return ext && (!_wcsicmp(ext, L".png") || !_wcsicmp(ext, L".jpg") || !_wcsicmp(ext, L".jpeg"));
}

static DWORD WINAPI WatchThread(LPVOID unused)
{
	(void)unused;
	HANDLE dir = CreateFileW(ShotDir, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	                         NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (dir == INVALID_HANDLE_VALUE) return 1;
	static DWORD buffer[8192];
	DWORD got = 0;
	while (ReadDirectoryChangesW(dir, buffer, sizeof buffer, FALSE, FILE_NOTIFY_CHANGE_FILE_NAME, &got, NULL, NULL)) {
		if (!got) continue;
		FILE_NOTIFY_INFORMATION *n = (FILE_NOTIFY_INFORMATION *)buffer;
		for (;;) {
			size_t len = n->FileNameLength / sizeof(wchar_t);
			if ((n->Action == FILE_ACTION_ADDED || n->Action == FILE_ACTION_RENAMED_NEW_NAME) && len < MAX_PATH / 2) {
				wchar_t name[MAX_PATH];
				wmemcpy(name, n->FileName, len);
				name[len] = 0;
				wchar_t *path = malloc(MAX_PATH * sizeof(wchar_t));
				if (path && IsImageName(name)) {
					swprintf(path, MAX_PATH, L"%ls\\%ls", ShotDir, name);
					PostMessageW(Peek, WM_NEWSHOT, 0, (LPARAM)path);
				} else {
					free(path);
				}
			}
			if (!n->NextEntryOffset) break;
			n = (FILE_NOTIFY_INFORMATION *)((BYTE *)n + n->NextEntryOffset);
		}
	}
	CloseHandle(dir);
	return 0;
}

//================
// Capture overlay: frozen, dimmed screen; drag a box; Esc cancels
//================

static HWND      Snip;                    // Overlay window (exists only while capturing)
static int       SnipX, SnipY, SnipW, SnipH; // Virtual-screen rectangle it covers
static uint32_t *SnipPixels;              // Frozen screen, top-down BGRA
static HDC       SnipBright, SnipDim, SnipBack; // Memory DCs: original, darkened, back buffer
static HBITMAP   SnipBrightBmp, SnipDimBmp, SnipBackBmp;
static HGDIOBJ   SnipOld[3];
static HFONT     SnipFont;
static bool      Selecting;
static POINT     SelFrom, SelTo;          // Client coordinates

static RECT SelRect(void)
{
	RECT r = {min(SelFrom.x, SelTo.x), min(SelFrom.y, SelTo.y), max(SelFrom.x, SelTo.x) + 1, max(SelFrom.y, SelTo.y) + 1};
	return r;
}

static HBITMAP MakeDib(HDC like, int w, int h, uint32_t **bits)
{
	BITMAPINFO bmi = {0};
	bmi.bmiHeader.biSize = sizeof bmi.bmiHeader; bmi.bmiHeader.biWidth = w; bmi.bmiHeader.biHeight = -h;
	bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32; bmi.bmiHeader.biCompression = BI_RGB;
	return CreateDIBSection(like, &bmi, DIB_RGB_COLORS, (void **)bits, NULL, 0);
}

static void EndSnip(void)
{
	if (!Snip) return;
	HWND window = Snip;
	Snip = NULL;
	Selecting = false;
	ReleaseCapture();
	DestroyWindow(window);
	SelectObject(SnipBright, SnipOld[0]); SelectObject(SnipDim, SnipOld[1]); SelectObject(SnipBack, SnipOld[2]);
	DeleteDC(SnipBright); DeleteDC(SnipDim); DeleteDC(SnipBack);
	DeleteObject(SnipBrightBmp); DeleteObject(SnipDimBmp); DeleteObject(SnipBackBmp);
	if (SnipFont) DeleteObject(SnipFont);
	SnipFont = NULL;
	SnipPixels = NULL; // Owned by SnipBrightBmp
}

static void CopyToClipboard(const uint32_t *top, int w, int h) // CF_DIB, bottom-up 32 bpp
{
	size_t bytes = sizeof(BITMAPINFOHEADER) + (size_t)w * h * 4;
	HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
	if (!mem) return;
	BITMAPINFOHEADER *hdr = GlobalLock(mem);
	*hdr = (BITMAPINFOHEADER){sizeof *hdr, w, h, 1, 32, BI_RGB, (DWORD)(w * h * 4)};
	uint32_t *dst = (uint32_t *)(hdr + 1);
	for (int y = 0; y < h; y++) memcpy(dst + (size_t)(h - 1 - y) * w, top + (size_t)y * w, (size_t)w * 4);
	GlobalUnlock(mem);
	if (OpenClipboard(Peek)) {
		EmptyClipboard();
		if (!SetClipboardData(CF_DIB, mem)) GlobalFree(mem);
		CloseClipboard();
	} else {
		GlobalFree(mem);
	}
}

static void FinishSnip(void)
{
	RECT r = SelRect();
	int w = r.right - r.left, h = r.bottom - r.top;
	if (w < 4 || h < 4) { EndSnip(); return; } // A click, not a box: cancel
	uint32_t *crop = malloc((size_t)w * h * 4);
	if (crop) for (int y = 0; y < h; y++) memcpy(crop + (size_t)y * w, SnipPixels + (size_t)(r.top + y) * SnipW + r.left, (size_t)w * 4);
	EndSnip(); // Overlay gone before the (slower) file write
	if (!crop) return;

	CopyToClipboard(crop, w, h);
	SYSTEMTIME t; GetLocalTime(&t);
	wchar_t path[MAX_PATH];
	swprintf(path, MAX_PATH, L"%ls\\Screenshot %04d-%02d-%02d %02d%02d%02d.png", ShotDir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
	for (int n = 2; GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES && n < 100; n++)
		swprintf(path, MAX_PATH, L"%ls\\Screenshot %04d-%02d-%02d %02d%02d%02d (%d).png", ShotDir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, n);
	void *bitmap = NULL;
	if (GdipCreateBitmapFromScan0(w, h, w * 4, PixelFormat32bppRGB, (BYTE *)crop, &bitmap) == 0 && bitmap) {
		GdipSaveImageToFile(bitmap, path, &PngEncoder, NULL); // The folder watcher then shows the preview
		GdipDisposeImage(bitmap);
	}
	free(crop);
}

static void PaintSnip(HDC target)
{
	BitBlt(SnipBack, 0, 0, SnipW, SnipH, SnipDim, 0, 0, SRCCOPY);
	if (Selecting) {
		RECT r = SelRect();
		int w = r.right - r.left, h = r.bottom - r.top;
		BitBlt(SnipBack, r.left, r.top, w, h, SnipBright, r.left, r.top, SRCCOPY);
		HBRUSH white = GetStockObject(WHITE_BRUSH);
		FrameRect(SnipBack, &r, white);

		// Size label in a dark pill below the box (above it near the bottom edge)
		wchar_t label[32];
		int len = swprintf(label, 32, L"%d × %d", w, h);
		HGDIOBJ oldFont = SelectObject(SnipBack, SnipFont);
		SIZE ts; GetTextExtentPoint32W(SnipBack, label, len, &ts);
		int padX = ts.cy / 2, padY = ts.cy / 5, gap = ts.cy / 2;
		int lw = ts.cx + 2 * padX, lh = ts.cy + 2 * padY;
		int lx = r.left, ly = r.bottom + gap;
		if (ly + lh > SnipH) ly = r.top - gap - lh;
		if (ly < 0) ly = r.top + gap;
		if (lx + lw > SnipW) lx = SnipW - lw;
		HBRUSH pill = CreateSolidBrush(RGB(32, 32, 32));
		HGDIOBJ oldBrush = SelectObject(SnipBack, pill), oldPen = SelectObject(SnipBack, GetStockObject(NULL_PEN));
		RoundRect(SnipBack, lx, ly, lx + lw + 1, ly + lh + 1, lh / 2, lh / 2);
		SelectObject(SnipBack, oldBrush); SelectObject(SnipBack, oldPen);
		DeleteObject(pill);
		SetBkMode(SnipBack, TRANSPARENT);
		SetTextColor(SnipBack, RGB(245, 245, 245));
		TextOutW(SnipBack, lx + padX, ly + padY, label, len);
		SelectObject(SnipBack, oldFont);
	}
	BitBlt(target, 0, 0, SnipW, SnipH, SnipBack, 0, 0, SRCCOPY);
}

static LRESULT CALLBACK SnipProcedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
	switch (message) {
		case WM_ERASEBKGND:
			return 1;
		case WM_PAINT: {
			PAINTSTRUCT ps;
			HDC dc = BeginPaint(hwnd, &ps);
			if (SnipBack) PaintSnip(dc);
			EndPaint(hwnd, &ps);
			return 0;
		}
		case WM_LBUTTONDOWN:
			SetCapture(hwnd);
			Selecting = true;
			SelFrom = SelTo = (POINT){GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
			InvalidateRect(hwnd, NULL, FALSE);
			return 0;
		case WM_MOUSEMOVE:
			if (Selecting) {
				SelTo.x = max(0, min(SnipW - 1, GET_X_LPARAM(lparam)));
				SelTo.y = max(0, min(SnipH - 1, GET_Y_LPARAM(lparam)));
				InvalidateRect(hwnd, NULL, FALSE);
			}
			return 0;
		case WM_LBUTTONUP:
			if (Selecting) FinishSnip();
			return 0;
		case WM_RBUTTONDOWN:
			EndSnip();
			return 0;
		case WM_KEYDOWN:
			if (wparam == VK_ESCAPE) EndSnip();
			return 0;
		case WM_ACTIVATE:
			if (LOWORD(wparam) == WA_INACTIVE && !Selecting) EndSnip(); // Switched away (Alt+Tab, Win key)
			return 0;
		case WM_DPICHANGED:
			return 0; // Keep covering the whole virtual screen as created
	}
	return DefWindowProcW(hwnd, message, wparam, lparam);
}

static void StartSnip(void)
{
	if (Snip) return;
	HideNow(); // Keep an old preview out of the shot
	SnipX = GetSystemMetrics(SM_XVIRTUALSCREEN); SnipY = GetSystemMetrics(SM_YVIRTUALSCREEN);
	SnipW = GetSystemMetrics(SM_CXVIRTUALSCREEN); SnipH = GetSystemMetrics(SM_CYVIRTUALSCREEN);

	HDC screen = GetDC(NULL);
	uint32_t *dim = NULL, *back = NULL;
	SnipBrightBmp = MakeDib(screen, SnipW, SnipH, &SnipPixels);
	SnipDimBmp = MakeDib(screen, SnipW, SnipH, &dim);
	SnipBackBmp = MakeDib(screen, SnipW, SnipH, &back);
	SnipBright = CreateCompatibleDC(screen); SnipDim = CreateCompatibleDC(screen); SnipBack = CreateCompatibleDC(screen);
	if (!SnipBrightBmp || !SnipDimBmp || !SnipBackBmp) {
		if (SnipBrightBmp) DeleteObject(SnipBrightBmp);
		if (SnipDimBmp) DeleteObject(SnipDimBmp);
		if (SnipBackBmp) DeleteObject(SnipBackBmp);
		DeleteDC(SnipBright); DeleteDC(SnipDim); DeleteDC(SnipBack);
		ReleaseDC(NULL, screen);
		return;
	}
	SnipOld[0] = SelectObject(SnipBright, SnipBrightBmp);
	SnipOld[1] = SelectObject(SnipDim, SnipDimBmp);
	SnipOld[2] = SelectObject(SnipBack, SnipBackBmp);
	BitBlt(SnipBright, 0, 0, SnipW, SnipH, screen, SnipX, SnipY, SRCCOPY | CAPTUREBLT);
	ReleaseDC(NULL, screen);
	GdiFlush();
	size_t count = (size_t)SnipW * SnipH;
	for (size_t i = 0; i < count; i++) {
		uint32_t p = SnipPixels[i] | 0xFF000000; // Opaque, for the PNG and clipboard
		SnipPixels[i] = p;
		dim[i] = 0xFF000000 | ((p >> 16 & 0xFF) * 140 / 255) << 16 | ((p >> 8 & 0xFF) * 140 / 255) << 8 | (p & 0xFF) * 140 / 255;
	}

	POINT mouse; GetCursorPos(&mouse);
	HMONITOR monitor = MonitorFromPoint(mouse, MONITOR_DEFAULTTOPRIMARY);
	UINT dpiX = 96, dpiY = 96;
	GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
	SnipFont = CreateFontW(-(int)(13 * dpiX / 96), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI Variable Text");

	Selecting = false;
	Snip = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"ShotPeekSnip", L"ShotPeek capture", WS_POPUP,
	                       SnipX, SnipY, SnipW, SnipH, NULL, NULL, GetModuleHandleW(NULL), NULL);
	if (!Snip) { // Free the buffers EndSnip would have freed
		SelectObject(SnipBright, SnipOld[0]); SelectObject(SnipDim, SnipOld[1]); SelectObject(SnipBack, SnipOld[2]);
		DeleteDC(SnipBright); DeleteDC(SnipDim); DeleteDC(SnipBack);
		DeleteObject(SnipBrightBmp); DeleteObject(SnipDimBmp); DeleteObject(SnipBackBmp);
		DeleteObject(SnipFont); SnipFont = NULL;
		return;
	}
	ShowWindow(Snip, SW_SHOW);
	SetForegroundWindow(Snip);
	SetFocus(Snip);
	UpdateWindow(Snip);
}

//================
// Window
//================

static LRESULT CALLBACK PeekProcedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
	switch (message) {
		case WM_HOTKEY:
			if (wparam == HOTKEY_F6) StartSnip();
			return 0;
		case WM_NEWSHOT: {
			wchar_t *path = (wchar_t *)lparam;
			wcsncpy(PendingPath, path, MAX_PATH - 1);
			free(path);
			PendingTries = 0;
			SetTimer(hwnd, TIMER_RETRY, 120, NULL); // Give the snipping tool a moment to finish writing
			return 0;
		}
		case WM_TIMER:
			if (wparam == TIMER_RETRY) {
				// The file may still be open for writing; retry for about 3 seconds
				HANDLE file = CreateFileW(PendingPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
				LARGE_INTEGER size = {0};
				bool ready = file != INVALID_HANDLE_VALUE && GetFileSizeEx(file, &size) && size.QuadPart > 0;
				if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
				if (ready || ++PendingTries > 25) {
					KillTimer(hwnd, TIMER_RETRY);
					if (ready) ShowShot(PendingPath);
				}
			} else if (wparam == TIMER_DISMISS) {
				if (Hovering || Pressed) return 0; // Stays while the mouse is on it
				Dismiss();
			}
			return 0;
		case WM_ANIMDONE:
			if ((LONG)wparam != AnimGen) return 0;
			if (lparam) ShowWindow(hwnd, SW_HIDE);
			else SetTimer(hwnd, TIMER_DISMISS, SHOW_MS, NULL); // Resting in the corner: full 6 s from now
			return 0;
		case WM_MOUSEACTIVATE:
			return MA_NOACTIVATE; // Never take focus from the app being worked in
		case WM_MOUSEMOVE: {
			if (!Hovering) {
				TRACKMOUSEEVENT track = {sizeof track, TME_LEAVE, hwnd, 0};
				TrackMouseEvent(&track);
				Hovering = true;
			}
			if (!Pressed) return 0;
			POINT now; GetCursorPos(&now);
			int dx = now.x - PressAt.x, dy = now.y - PressAt.y;
			int slop = 6 * Scale;
			if (!Swiping && (abs(dx) > slop || abs(dy) > slop)) {
				if (dx > 0 && dx > 2 * abs(dy)) {
					Swiping = true; // Rightward flick: dismiss gesture
					StopAnimation();
				} else {
					Pressed = false; // Anything else: drag the file out
					ReleaseCapture();
					DragShot();
					return 0;
				}
			}
			if (Swiping) Place(PeekHome.x + (dx > 0 ? dx : 0));
			return 0;
		}
		case WM_MOUSELEAVE:
			Hovering = false;
			return 0;
		case WM_LBUTTONDOWN:
			SetCapture(hwnd);
			Pressed = true;
			Swiping = false;
			GetCursorPos(&PressAt);
			return 0;
		case WM_LBUTTONUP: {
			bool wasPressed = Pressed, wasSwiping = Swiping;
			Pressed = Swiping = false;
			ReleaseCapture();
			if (wasSwiping) {
				if (PeekX - PeekHome.x > 40 * Scale) Dismiss();
				else Animate(PeekHome.x, 180, false);
			} else if (wasPressed) {
				OpenShot();
				HideNow();
			}
			return 0;
		}
		case WM_CAPTURECHANGED:
			if (Swiping) { Swiping = Pressed = false; Animate(PeekHome.x, 180, false); }
			return 0;
		case WM_RBUTTONUP:
			ShowMenu();
			return 0;
		case WM_DESTROY:
			UnregisterHotKey(hwnd, HOTKEY_F6);
			PostQuitMessage(0);
			return 0;
	}
	return DefWindowProcW(hwnd, message, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR args, int show)
{
	(void)previous; (void)show;
	CreateMutexW(NULL, TRUE, L"ShotPeekSingleInstance");
	if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	OleInitialize(NULL);
	ULONG_PTR token;
	GpStartupInput gdiplus = {1, NULL, FALSE, FALSE};
	GdiplusStartup(&token, &gdiplus, NULL);

	// Snipping Tool's save folder (Pictures\Screenshots)
	PWSTR known = NULL;
	if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Screenshots, KF_FLAG_CREATE, NULL, &known))) {
		wcsncpy(ShotDir, known, MAX_PATH - 1);
		CoTaskMemFree(known);
	} else {
		return 1;
	}

	WNDCLASSEXW wc = {0};
	wc.cbSize = sizeof wc;
	wc.lpfnWndProc = PeekProcedure;
	wc.hInstance = instance;
	wc.hCursor = LoadCursorW(NULL, IDC_HAND);
	wc.lpszClassName = L"ShotPeek";
	RegisterClassExW(&wc);
	Peek = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"ShotPeek", L"ShotPeek",
	                       WS_POPUP, 0, 0, 0, 0, NULL, NULL, instance, NULL);
	if (!Peek) return 1;

	WNDCLASSEXW snip = {0};
	snip.cbSize = sizeof snip;
	snip.lpfnWndProc = SnipProcedure;
	snip.hInstance = instance;
	snip.hCursor = LoadCursorW(NULL, IDC_CROSS);
	snip.lpszClassName = L"ShotPeekSnip";
	RegisterClassExW(&snip);

	if (!RegisterHotKey(Peek, HOTKEY_F6, MOD_NOREPEAT, VK_F6) && !(args && wcsstr(args, L"--autorun"))) {
		MessageBoxW(NULL, L"F6 is already taken by another app, so ShotPeek can't use it.\nScreenshots taken with Win+Shift+S still show the preview.", L"ShotPeek", MB_ICONWARNING);
	}

	HANDLE watcher = CreateThread(NULL, 0, WatchThread, NULL, 0, NULL);
	if (watcher) CloseHandle(watcher);

	// A path on the command line shows it straight away (used for testing)
	if (args && *args && args[0] != L'-') {
		wchar_t path[MAX_PATH];
		wcsncpy(path, args[0] == L'"' ? args + 1 : args, MAX_PATH - 1);
		path[MAX_PATH - 1] = 0;
		wchar_t *quote = wcschr(path, L'"');
		if (quote) *quote = 0;
		ShowShot(path);
	}

	MSG msg;
	while (GetMessageW(&msg, NULL, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	OleUninitialize();
	return 0;
}
