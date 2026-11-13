#include "stdafx.h"
#include "AiAssistant.h"
#include "AiDialogs.h"
#include "AiRag.h"
#include "AiUsage.h"
#include "Misc.h"
#include "Options.h"

#include <algorithm>
#include <memory>
#include <wincrypt.h>

namespace
{
	const UINT_PTR TIMER_READ_CLIPBOARD = 1;
	const UINT_PTR TIMER_SAVE_FALLBACK = 2;
	const UINT_PTR TIMER_PUMP_CLIPBOARD = 3;

	const UINT READ_CLIPBOARD_DELAY_MS = 200;	// let the source app finish its copy
	const UINT SAVE_FALLBACK_MS = 5000;			// how long to wait for Ditto to save the answer
	const UINT CLIPBOARD_RETRY_MS = 50;			// another program has the clipboard open: try again after this
	const ULONGLONG CLIPBOARD_WAIT_MS = 2000;	// ... until this much time has passed

	const int MAX_CLIP_CHARS = 400000;
	const int MAX_IMAGE_EDGE = 1568;			// larger images are scaled down before sending
	const int MAX_RECENT_RUNS = 10;				// finished requests kept for the requests window

	// hook state, used by the hook thread only (plus values set before the thread starts)
	HHOOK s_hHook = NULL;
	HWND s_notifyWnd = NULL;
	DWORD s_intervalMs = 400;
	DWORD s_lastCopyTime = 0;
	bool s_cHeld = false;

	CString OneLine(CString text, int maxChars)
	{
		text.Replace(_T("\r"), _T(" "));
		text.Replace(_T("\n"), _T(" "));
		text.Replace(_T("\t"), _T(" "));
		while (text.Replace(_T("  "), _T(" ")) > 0) {}
		text.Trim();
		if (text.GetLength() > maxChars)
		{
			text = text.Left(maxChars) + _T("…");
		}
		return text;
	}

	// Name of the program that has the clipboard open, empty if it cannot be told.
	CString ClipboardHolderName()
	{
		HWND hWnd = ::GetOpenClipboardWindow();
		DWORD pid = 0;
		if (hWnd == NULL || ::GetWindowThreadProcessId(hWnd, &pid) == 0 || pid == 0)
		{
			return CString();
		}
		if (pid == ::GetCurrentProcessId())
		{
			return _T("Ditto");
		}

		CString name;
		HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (process != NULL)
		{
			TCHAR path[MAX_PATH];
			DWORD size = MAX_PATH;
			if (::QueryFullProcessImageName(process, 0, path, &size))
			{
				name = path;
				name = name.Mid(name.ReverseFind(_T('\\')) + 1);
			}
			::CloseHandle(process);
		}
		if (name.IsEmpty())
		{
			name.Format(_T("işlem %u"), pid);
		}
		return name;
	}

	// Error text after the clipboard stayed locked for CLIPBOARD_WAIT_MS; also written to Ditto's log.
	CString ClipboardBusyText()
	{
		CString holder = ClipboardHolderName();
		CString text;
		text.Format(_T("Pano %d saniye boyunca açılamadı, şu an %s tarafından kullanılıyor."), (int)(CLIPBOARD_WAIT_MS / 1000),
			holder.IsEmpty() ? _T("başka bir uygulama") : (LPCTSTR)holder);
		Log(_T("AI: ") + text);
		return text;
	}

	void ForceForeground(HWND hWnd)
	{
		if (hWnd == NULL || !::IsWindow(hWnd))
		{
			return;
		}

		HWND foreground = ::GetForegroundWindow();
		DWORD foregroundThread = foreground ? ::GetWindowThreadProcessId(foreground, NULL) : 0;
		DWORD currentThread = ::GetCurrentThreadId();

		bool attached = false;
		if (foregroundThread != 0 && foregroundThread != currentThread)
		{
			attached = ::AttachThreadInput(foregroundThread, currentThread, TRUE) != FALSE;
		}

		::BringWindowToTop(hWnd);
		::SetForegroundWindow(hWnd);

		if (attached)
		{
			::AttachThreadInput(foregroundThread, currentThread, FALSE);
		}
	}

	// ---------------------------------------------------------------- images

	bool GetPngEncoder(CLSID &clsid)
	{
		UINT count = 0, size = 0;
		if (Gdiplus::GetImageEncodersSize(&count, &size) != Gdiplus::Ok || size == 0)
		{
			return false;
		}
		std::vector<BYTE> buffer(size);
		Gdiplus::ImageCodecInfo *codecs = (Gdiplus::ImageCodecInfo *)buffer.data();
		Gdiplus::GetImageEncoders(count, size, codecs);
		for (UINT i = 0; i < count; i++)
		{
			if (wcscmp(codecs[i].MimeType, L"image/png") == 0)
			{
				clsid = codecs[i].Clsid;
				return true;
			}
		}
		return false;
	}

	// Loads the clipboard image into a GDI+ bitmap (PNG format first, then DIB).
	std::unique_ptr<Gdiplus::Bitmap> LoadClipboardBitmap()
	{
		UINT pngFormat = ::RegisterClipboardFormat(_T("PNG"));
		HANDLE hPng = ::IsClipboardFormatAvailable(pngFormat) ? ::GetClipboardData(pngFormat) : NULL;
		if (hPng != NULL)
		{
			SIZE_T size = ::GlobalSize(hPng);
			const void *data = ::GlobalLock(hPng);
			if (data != NULL && size > 0)
			{
				HGLOBAL copy = ::GlobalAlloc(GMEM_MOVEABLE, size);
				if (copy != NULL)
				{
					memcpy(::GlobalLock(copy), data, size);
					::GlobalUnlock(copy);
					IStream *stream = NULL;
					if (SUCCEEDED(::CreateStreamOnHGlobal(copy, TRUE, &stream)))
					{
						std::unique_ptr<Gdiplus::Bitmap> clone;
						{
							std::unique_ptr<Gdiplus::Bitmap> bitmap(Gdiplus::Bitmap::FromStream(stream));
							if (bitmap && bitmap->GetLastStatus() == Gdiplus::Ok)
							{
								// detach from the stream by drawing into a new bitmap
								clone.reset(new Gdiplus::Bitmap(bitmap->GetWidth(), bitmap->GetHeight(), PixelFormat32bppARGB));
								Gdiplus::Graphics g(clone.get());
								g.DrawImage(bitmap.get(), 0, 0, bitmap->GetWidth(), bitmap->GetHeight());
							}
						}
						stream->Release();
						::GlobalUnlock(hPng);
						if (clone)
						{
							return clone;
						}
						hPng = NULL;
					}
					else
					{
						::GlobalFree(copy);
					}
				}
			}
			if (hPng != NULL)
			{
				::GlobalUnlock(hPng);
			}
		}

		HANDLE hDib = ::GetClipboardData(CF_DIB);
		if (hDib == NULL)
		{
			return nullptr;
		}
		BITMAPINFO *info = (BITMAPINFO *)::GlobalLock(hDib);
		if (info == NULL)
		{
			return nullptr;
		}

		const BITMAPINFOHEADER &h = info->bmiHeader;
		int colors = h.biClrUsed;
		if (colors == 0 && h.biBitCount <= 8)
		{
			colors = 1 << h.biBitCount;
		}
		int masks = (h.biCompression == BI_BITFIELDS && h.biSize == sizeof(BITMAPINFOHEADER)) ? 3 : 0;
		BYTE *bits = (BYTE *)info + h.biSize + (colors + masks) * sizeof(RGBQUAD);

		std::unique_ptr<Gdiplus::Bitmap> result;
		Gdiplus::Bitmap source(info, bits);
		if (source.GetLastStatus() == Gdiplus::Ok && source.GetWidth() > 0)
		{
			result.reset(new Gdiplus::Bitmap(source.GetWidth(), source.GetHeight(), PixelFormat32bppARGB));
			Gdiplus::Graphics g(result.get());
			g.DrawImage(&source, 0, 0, source.GetWidth(), source.GetHeight());
		}
		::GlobalUnlock(hDib);
		return result;
	}

	bool EncodeImage(Gdiplus::Bitmap *bitmap, AiImage &image)
	{
		UINT width = bitmap->GetWidth(), height = bitmap->GetHeight();
		image.width = width;
		image.height = height;

		std::unique_ptr<Gdiplus::Bitmap> scaled;
		Gdiplus::Bitmap *toEncode = bitmap;
		UINT longEdge = max(width, height);
		if (longEdge > MAX_IMAGE_EDGE)
		{
			double factor = (double)MAX_IMAGE_EDGE / longEdge;
			UINT w = max(1u, (UINT)(width * factor)), hgt = max(1u, (UINT)(height * factor));
			scaled.reset(new Gdiplus::Bitmap(w, hgt, PixelFormat32bppARGB));
			Gdiplus::Graphics g(scaled.get());
			g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
			g.DrawImage(bitmap, 0, 0, w, hgt);
			toEncode = scaled.get();
		}

		CLSID pngClsid;
		if (!GetPngEncoder(pngClsid))
		{
			return false;
		}

		IStream *stream = NULL;
		if (FAILED(::CreateStreamOnHGlobal(NULL, TRUE, &stream)))
		{
			return false;
		}
		bool ok = false;
		if (toEncode->Save(stream, &pngClsid, NULL) == Gdiplus::Ok)
		{
			STATSTG stat = { 0 };
			HGLOBAL hGlobal = NULL;
			if (SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)) && SUCCEEDED(::GetHGlobalFromStream(stream, &hGlobal)))
			{
				DWORD size = (DWORD)stat.cbSize.QuadPart;
				const BYTE *data = (const BYTE *)::GlobalLock(hGlobal);
				DWORD chars = 0;
				::CryptBinaryToStringA(data, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &chars);
				std::string base64(chars, '\0');
				if (::CryptBinaryToStringA(data, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &base64[0], &chars))
				{
					base64.resize(chars);
					image.base64 = base64;
					image.mediaType = _T("image/png");
					ok = true;
				}
				::GlobalUnlock(hGlobal);
			}
		}
		stream->Release();
		return ok;
	}

	// ---------------------------------------------------------------- request

	struct AiJob
	{
		HWND notifyWnd;
		int runId;
		AiCallInput input;
	};
}

// ===========================================================================

BEGIN_MESSAGE_MAP(CAiAssistant, CWnd)
	ON_MESSAGE(WM_AI_DOUBLE_COPY, OnDoubleCopy)
	ON_MESSAGE(WM_AI_PICKED, OnPicked)
	ON_MESSAGE(WM_AI_RESULT, OnAiResult)
	ON_MESSAGE(WM_AI_CLIP_SAVED, OnAiClipSaved)
	ON_MESSAGE(WM_AI_TOAST_CLICKED, OnToastClicked)
	ON_WM_TIMER()
END_MESSAGE_MAP()

CAiAssistant &CAiAssistant::Instance()
{
	static CAiAssistant instance;
	return instance;
}

CAiAssistant::CAiAssistant()
	: m_targetWnd(NULL), m_inSettings(false), m_readPending(false), m_readStarted(0), m_runCounter(0), m_savingRunId(0), m_lastClipWrite(0), m_writeStarted(0), m_awaitingSave(0),
	m_runsDlg([this](int runId) { ShowRunDetail(runId); }), m_hookThread(NULL), m_hookThreadId(0)
{
}

CAiAssistant::~CAiAssistant()
{
	StopHook();
}

void CAiAssistant::Start()
{
	if (m_hWnd == NULL)
	{
		// hidden top level window: owns dialogs/popups and receives the hook messages
		CString className = AfxRegisterWndClass(0);
		if (!CreateEx(WS_EX_TOOLWINDOW, className, _T("Ditto AI"), WS_POPUP, CRect(0, 0, 0, 0), NULL, 0))
		{
			Log(_T("AI: failed to create the controller window"));
			return;
		}
	}
	m_toasts.SetClickTarget(m_hWnd);

	if (!m_config.Load())
	{
		// first run: write the defaults so the file can be inspected/backed up
		m_config.Save();
	}
	ApplySettings();
}

void CAiAssistant::Stop()
{
	StopHook();
	InterlockedExchange(&m_awaitingSave, 0);

	if (m_picker.m_hWnd != NULL)
	{
		m_picker.DestroyWindow();
	}
	if (m_detail.m_hWnd != NULL)
	{
		m_detail.DestroyWindow();
	}
	if (m_runsDlg.m_hWnd != NULL)
	{
		m_runsDlg.DestroyWindow();
	}
	m_toasts.DestroyAll();
	if (m_hWnd != NULL)
	{
		DestroyWindow();
	}
}

void CAiAssistant::ApplySettings()
{
	StopHook();
	if (m_config.enabled)
	{
		StartHook();
	}
}

void CAiAssistant::ShowSettings(int startPage)
{
	if (m_hWnd == NULL || m_inSettings)
	{
		return;
	}

	m_picker.Close();
	m_inSettings = true;

	AiConfig edited = m_config;
	auto preview = [this](const AiNotifySettings &settings)
	{
		ShowToast(settings, AiToastKind::Success, _T("Örnek bildirim - cevap hazır"),
			settings.showPreview ? _T("Kurumsal e-postaya çevir · Ditto'ya kaydedildi\n\nSayın ilgili, talebinizle ilgili bilgileri ekte paylaşıyorum...") : _T("Kurumsal e-postaya çevir · Ditto'ya kaydedildi"),
			false);
	};

	CAiSettingsDlg dlg(edited, preview, this, startPage);
	if (dlg.DoModal() == IDOK)
	{
		m_config = edited;
		CString error;
		if (!m_config.Save(&error))
		{
			AfxMessageBox(error, MB_ICONERROR);
		}
		ApplySettings();
	}

	m_inSettings = false;
}

void CAiAssistant::ShowRequests()
{
	if (m_hWnd != NULL)
	{
		m_runsDlg.Show(RunList(), this);
	}
}

void CAiAssistant::AddMenuItems(CMenu *pMenu, UINT beforeId)
{
	if (pMenu == NULL || pMenu->GetMenuState(ID_AI_SETTINGS, MF_BYCOMMAND) != (UINT)-1)
	{
		return;
	}

	struct { UINT id; LPCTSTR text; } items[] =
	{
		{ ID_AI_SETTINGS, _T("Yapay zeka ayarları...") },
		{ ID_AI_REQUESTS, _T("Yapay zeka istekleri...") },
	};
	bool hasAnchor = pMenu->GetMenuState(beforeId, MF_BYCOMMAND) != (UINT)-1;
	for (int i = 0; i < _countof(items); i++)
	{
		if (hasAnchor)
		{
			pMenu->InsertMenu(beforeId, MF_BYCOMMAND | MF_STRING, items[i].id, items[i].text);
		}
		else
		{
			pMenu->InsertMenu(i, MF_BYPOSITION | MF_STRING, items[i].id, items[i].text);
		}
	}
}

// ---------------------------------------------------------------- keyboard hook

void CAiAssistant::StartHook()
{
	if (m_hookThread != NULL)
	{
		return;
	}

	s_notifyWnd = m_hWnd;
	s_intervalMs = (DWORD)m_config.doubleCopyMs;
	s_lastCopyTime = 0;
	s_cHeld = false;

	HANDLE ready = ::CreateEvent(NULL, TRUE, FALSE, NULL);
	m_hookThread = ::CreateThread(NULL, 0, HookThreadProc, ready, 0, &m_hookThreadId);
	if (m_hookThread == NULL)
	{
		Log(_T("AI: failed to start the keyboard hook thread"));
		::CloseHandle(ready);
		return;
	}

	::WaitForSingleObject(ready, 5000);
	::CloseHandle(ready);
}

void CAiAssistant::StopHook()
{
	if (m_hookThread == NULL)
	{
		return;
	}

	::PostThreadMessage(m_hookThreadId, WM_QUIT, 0, 0);
	::WaitForSingleObject(m_hookThread, 3000);
	::CloseHandle(m_hookThread);
	m_hookThread = NULL;
	m_hookThreadId = 0;
}

// The low level hook runs on its own thread so a busy UI thread can never delay
// keyboard input or get the hook removed by Windows for timing out.
DWORD WINAPI CAiAssistant::HookThreadProc(LPVOID param)
{
	MSG msg;
	::PeekMessage(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);	// create the message queue

	s_hHook = ::SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, ::GetModuleHandle(NULL), 0);
	if (s_hHook == NULL)
	{
		Log(StrF(_T("AI: SetWindowsHookEx failed, error %d"), ::GetLastError()));
	}

	::SetEvent((HANDLE)param);

	while (::GetMessage(&msg, NULL, 0, 0) > 0)
	{
		::TranslateMessage(&msg);
		::DispatchMessage(&msg);
	}

	if (s_hHook != NULL)
	{
		::UnhookWindowsHookEx(s_hHook);
		s_hHook = NULL;
	}
	return 0;
}

LRESULT CALLBACK CAiAssistant::LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
	if (nCode == HC_ACTION)
	{
		const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *)lParam;
		bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
		bool up = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);

		// Ignore synthetic input, e.g. Ditto itself sending Ctrl+C.
		if ((k->flags & LLKHF_INJECTED) == 0)
		{
			if (k->vkCode == 'C')
			{
				if (down && !s_cHeld)
				{
					s_cHeld = true;

					bool ctrl = (::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
					bool otherModifier = (::GetAsyncKeyState(VK_SHIFT) & 0x8000) || (::GetAsyncKeyState(VK_MENU) & 0x8000) ||
						(::GetAsyncKeyState(VK_LWIN) & 0x8000) || (::GetAsyncKeyState(VK_RWIN) & 0x8000);

					if (ctrl && !otherModifier)
					{
						if (s_lastCopyTime != 0 && (k->time - s_lastCopyTime) <= s_intervalMs)
						{
							s_lastCopyTime = 0;
							::PostMessage(s_notifyWnd, WM_AI_DOUBLE_COPY, 0, 0);
						}
						else
						{
							s_lastCopyTime = k->time ? k->time : 1;
						}
					}
					else
					{
						s_lastCopyTime = 0;
					}
				}
				else if (up)
				{
					s_cHeld = false;
				}
			}
			else if (down)
			{
				switch (k->vkCode)
				{
				case VK_CONTROL:
				case VK_LCONTROL:
				case VK_RCONTROL:
					break;
				default:
					s_lastCopyTime = 0;		// any other key breaks the double press
					break;
				}
			}
		}
	}

	return ::CallNextHookEx(NULL, nCode, wParam, lParam);
}

// ---------------------------------------------------------------- flow

LRESULT CAiAssistant::OnDoubleCopy(WPARAM wParam, LPARAM lParam)
{
	if (m_inSettings || m_picker.IsOpen())
	{
		return 0;
	}

	m_targetWnd = ::GetForegroundWindow();

	// the second Ctrl+C is still being handled by the source app, give it a moment;
	// meanwhile answers of other requests must not replace the copied content
	m_readPending = true;
	m_readStarted = 0;
	KillTimer(TIMER_READ_CLIPBOARD);
	SetTimer(TIMER_READ_CLIPBOARD, READ_CLIPBOARD_DELAY_MS, NULL);
	return 0;
}

void CAiAssistant::SendClip(const CString &text, Gdiplus::Bitmap *image, HWND targetWnd)
{
	if (m_hWnd == NULL || m_inSettings || m_picker.IsOpen())
	{
		return;
	}

	ClipContent content;
	CString trimmed = text;
	trimmed.Trim();
	if (trimmed.IsEmpty() == FALSE)
	{
		if (text.GetLength() > MAX_CLIP_CHARS)
		{
			Notify(NotifyKind::Error, _T("Gönderilecek içerik çok uzun"),
				StrF(_T("%d karakter seçildi, en fazla %d karakter gönderilebilir."), text.GetLength(), MAX_CLIP_CHARS));
			return;
		}
		content.text = text;
	}
	else if (image != nullptr)
	{
		if (!EncodeImage(image, content.image))
		{
			Notify(NotifyKind::Error, _T("Gönderilecek içerik yok"), _T("Görsel PNG'ye dönüştürülemedi."));
			return;
		}
		content.isImage = true;
	}
	else
	{
		Notify(NotifyKind::Error, _T("Gönderilecek içerik yok"), _T("Seçilen kayıtta metin ya da görsel yok."));
		return;
	}

	m_targetWnd = targetWnd;
	m_clip = content;
	ShowPicker();
}

void CAiAssistant::OnTimer(UINT_PTR nIDEvent)
{
	if (nIDEvent == TIMER_READ_CLIPBOARD)
	{
		KillTimer(TIMER_READ_CLIPBOARD);

		CString error;
		ClipContent content;
		bool locked = false;
		bool read = ReadClipboard(content, error, locked);
		if (locked)
		{
			// another program is reading the clipboard (e.g. Ditto itself or a clipboard sync tool)
			ULONGLONG now = ::GetTickCount64();
			if (m_readStarted == 0)
			{
				m_readStarted = now;
			}
			if (now - m_readStarted < CLIPBOARD_WAIT_MS)
			{
				SetTimer(TIMER_READ_CLIPBOARD, CLIPBOARD_RETRY_MS, NULL);
				return;
			}
			error = ClipboardBusyText();
		}
		m_readStarted = 0;
		m_readPending = false;
		PumpClipboard();	// answers that arrived meanwhile
		if (!read)
		{
			Notify(NotifyKind::Error, _T("Gönderilecek içerik yok"), error);
			return;
		}
		m_clip = content;
		ShowPicker();
		return;
	}

	if (nIDEvent == TIMER_SAVE_FALLBACK)
	{
		KillTimer(TIMER_SAVE_FALLBACK);
		if (InterlockedExchange(&m_awaitingSave, 0) != 0)
		{
			// Ditto did not report saving it (e.g. clipboard monitoring is off); the text is on the clipboard anyway.
			FinishSave(false);
		}
		return;
	}

	if (nIDEvent == TIMER_PUMP_CLIPBOARD)
	{
		KillTimer(TIMER_PUMP_CLIPBOARD);
		PumpClipboard();
		return;
	}

	CWnd::OnTimer(nIDEvent);
}

bool CAiAssistant::ReadClipboard(ClipContent &content, CString &error, bool &locked)
{
	// one attempt only: the caller retries with a timer so Ditto never freezes while waiting
	locked = ::OpenClipboard(m_hWnd) == FALSE;
	if (locked)
	{
		return false;
	}

	HANDLE hText = ::GetClipboardData(CF_UNICODETEXT);
	if (hText != NULL)
	{
		LPCWSTR p = (LPCWSTR)::GlobalLock(hText);
		if (p != NULL)
		{
			content.text = p;
			::GlobalUnlock(hText);
		}
	}

	CString trimmed = content.text;
	trimmed.Trim();
	if (trimmed.IsEmpty())
	{
		content.text.Empty();
		std::unique_ptr<Gdiplus::Bitmap> bitmap = LoadClipboardBitmap();
		::CloseClipboard();

		if (!bitmap)
		{
			error = _T("Panoda metin ya da görsel yok.");
			return false;
		}
		if (!EncodeImage(bitmap.get(), content.image))
		{
			error = _T("Görsel PNG'ye dönüştürülemedi.");
			return false;
		}
		content.isImage = true;
		return true;
	}

	::CloseClipboard();

	if (content.text.GetLength() > MAX_CLIP_CHARS)
	{
		error.Format(_T("%d karakter kopyalandı, en fazla %d karakter gönderilebilir."), content.text.GetLength(), MAX_CLIP_CHARS);
		return false;
	}
	return true;
}

void CAiAssistant::ShowPicker()
{
	bool image = m_clip.isImage;

	std::vector<AiPickerItem> items;
	for (int i = 0; i < (int)m_config.actions.size(); i++)
	{
		const AiAction &action = m_config.actions[i];
		if (!action.AppliesTo(image))
		{
			continue;
		}
		const AiModelConfig *model = m_config.ResolveModel(action, image);

		AiPickerItem item;
		item.actionIndex = i;
		item.title = action.name;
		item.detail = model ? model->name : CString(_T("model seçilmemiş"));
		if (model && image && !model->supportsImages)
		{
			item.detail += _T(" (görsel desteklemiyor)");
		}
		item.searchText = action.instruction;
		items.push_back(item);
	}

	const AiModelConfig *defaultModel = m_config.DefaultModelFor(image);
	CString customDetail = defaultModel ? defaultModel->name : CString(_T("model seçilmemiş"));

	CString header;
	if (image)
	{
		header.Format(_T("Görsel: %d × %d piksel"), m_clip.image.width, m_clip.image.height);
	}
	else
	{
		header.Format(_T("Metin (%d karakter): "), m_clip.text.GetLength());
		header += _T("\"") + OneLine(m_clip.text, 70) + _T("\"");
	}

	CPoint pt;
	::GetCursorPos(&pt);
	m_picker.Show(this, header, items, customDetail, pt);
}

LRESULT CAiAssistant::OnPicked(WPARAM wParam, LPARAM lParam)
{
	std::unique_ptr<AiPickResult> result((AiPickResult *)lParam);
	if (!result)
	{
		ForceForeground(m_targetWnd);
		return 0;
	}

	bool image = m_clip.isImage;

	if (result->kind == AiPickResult::Settings)
	{
		ShowSettings(0);
		ForceForeground(m_targetWnd);
		return 0;
	}

	const AiModelConfig *model = nullptr;
	CString title, instruction;

	if (result->kind == AiPickResult::Custom)
	{
		instruction = result->customInstruction;
		title = OneLine(instruction, 40);
		model = m_config.DefaultModelFor(image);
	}
	else
	{
		if (result->actionIndex < 0 || result->actionIndex >= (int)m_config.actions.size())
		{
			return 0;
		}
		const AiAction &action = m_config.actions[result->actionIndex];
		title = action.name;
		instruction = action.instruction;
		model = m_config.ResolveModel(action, image);
	}

	ForceForeground(m_targetWnd);

	if (instruction.IsEmpty())
	{
		return 0;
	}
	if (model == nullptr)
	{
		Notify(NotifyKind::Error, _T("Model seçilmemiş"), _T("Tepsi menüsü > Yapay zeka ayarları > Modeller sekmesinden bir model ekleyin."));
		return 0;
	}
	if (image && !model->supportsImages)
	{
		Notify(NotifyKind::Error, _T("Model görsel desteklemiyor"),
			_T("'") + model->name + _T("' görsel kabul etmiyor. Eylemi görsel destekleyen bir modele yönlendirin ya da model ayarlarında 'Görsel girdi destekler'i işaretleyin."));
		return 0;
	}

	Run(title, instruction, *model);
	return 0;
}

void CAiAssistant::Run(const CString &title, const CString &instruction, const AiModelConfig &model)
{
	CString apiKey = model.ResolveApiKey();
	if (apiKey.IsEmpty() && model.RequiresApiKey())
	{
		Notify(NotifyKind::Error, _T("API anahtarı yok"),
			_T("'") + model.name + _T("' için ayarlardan bir API anahtarı girin ya da ") + model.EnvironmentKeyName() + _T(" ortam değişkenini tanımlayın."));
		ShowSettings(0);
		ForceForeground(m_targetWnd);
		return;
	}

	CString provider = AiModelConfig::ProviderName(model.provider);
	int paren = provider.Find(_T(" ("));
	if (paren > 0)
	{
		provider = provider.Left(paren);
	}

	RunState run;
	run.model = model;
	AiRunInfo &info = run.info;
	info.valid = true;
	info.id = ++m_runCounter;
	info.title = title;
	info.instruction = instruction;
	info.modelName = model.name;
	info.modelDetail = provider + _T(" · ") + model.model;
	info.isImage = m_clip.isImage;
	info.imageWidth = m_clip.image.width;
	info.imageHeight = m_clip.image.height;
	info.inputText = m_clip.isImage ? CString() : m_clip.text;
	info.started = CTime::GetCurrentTime();
	info.startTick = ::GetTickCount64();
	int id = info.id;
	m_runs.push_back(std::move(run));

	AiJob *job = new AiJob();
	job->notifyWnd = m_hWnd;
	job->runId = id;
	job->input.model = model;
	job->input.apiKey = apiKey;
	job->input.instruction = instruction;
	job->input.text = m_clip.isImage ? CString() : m_clip.text;
	job->input.image = m_clip.image;

	CString what = m_clip.isImage ? StrF(_T("Görsel %d × %d"), m_clip.image.width, m_clip.image.height) : OneLine(m_clip.text, 120);
	Notify(NotifyKind::Progress, model.name + _T(" çalışıyor..."), title + _T("\n") + what, id);

	if (AfxBeginThread(RequestThreadProc, job) == NULL)
	{
		delete job;
		RunState *started = FindRun(id);
		started->info.state = AiRunInfo::State::Error;
		started->info.error = _T("İstek başlatılamadı.");
		Notify(NotifyKind::Error, _T("İstek başlatılamadı"), CString(), id);
	}
	RunChanged(id);
}

UINT CAiAssistant::RequestThreadProc(LPVOID param)
{
	std::unique_ptr<AiJob> job((AiJob *)param);

	if (job->input.model.ragEnabled)
	{
		job->input.ragContext = AiRagRetrieve(job->input.model, job->input.instruction + _T("\n") + job->input.text);
	}

	AiCallResult *result = new AiCallResult(AiCallModel(job->input));
	if (!::PostMessage(job->notifyWnd, WM_AI_RESULT, (WPARAM)job->runId, (LPARAM)result))
	{
		delete result;	// Ditto is closing
	}
	return 0;
}

LRESULT CAiAssistant::OnAiResult(WPARAM wParam, LPARAM lParam)
{
	std::unique_ptr<AiCallResult> result((AiCallResult *)lParam);
	int id = (int)wParam;
	RunState *run = FindRun(id);
	if (run == nullptr)
	{
		return 0;	// running requests are never dropped, so this does not happen
	}

	// a refused or empty answer still used tokens if the provider reported them
	if (result->ok || result->usageKnown)
	{
		AiUsageRecord(run->model, result->usageKnown, result->inputTokens, result->outputTokens);
	}

	AiRunInfo &info = run->info;
	info.durationMs = ::GetTickCount64() - info.startTick;
	info.usageKnown = result->usageKnown;
	info.inputTokens = result->inputTokens;
	info.outputTokens = result->outputTokens;

	if (!result->ok)
	{
		info.state = AiRunInfo::State::Error;
		info.error = result->error;
		Notify(NotifyKind::Error, _T("Model cevap veremedi"), result->error, id);
		RunChanged(id);
		return 0;
	}

	CString text = result->text;
	text.Replace(_T("\r\n"), _T("\n"));
	text.Replace(_T("\n"), _T("\r\n"));

	info.state = AiRunInfo::State::Success;
	info.result = text;
	info.warning = result->warning;
	info.clipboardStatus = _T("Panoya yazılmak için sırada");

	m_clipQueue.push_back(id);
	PumpClipboard();	// may drop old runs, run is not used after this
	RunChanged(id);
	return 0;
}

// Answers go to the clipboard one at a time: the next one is written only after Ditto
// saved the previous one, otherwise Ditto could miss an answer that was replaced too quickly.
void CAiAssistant::PumpClipboard()
{
	while (m_savingRunId == 0 && !m_readPending && !m_clipQueue.empty())
	{
		// Ditto drops a clipboard change that comes within SaveClipDelay of the copy before it
		ULONGLONG gap = CGetSetOptions::m_dwSaveClipDelay + 200;
		ULONGLONG since = ::GetTickCount64() - m_lastClipWrite;
		if (m_lastClipWrite != 0 && since < gap)
		{
			SetTimer(TIMER_PUMP_CLIPBOARD, (UINT)(gap - since), NULL);
			return;
		}

		int id = m_clipQueue.front();
		m_clipQueue.pop_front();
		RunState *run = FindRun(id);
		if (run == nullptr)
		{
			continue;
		}

		// waiting for Ditto's save thread to report the new clip
		InterlockedExchange(&m_awaitingSave, 1);

		if (!WriteClipboardText(run->info.result))
		{
			InterlockedExchange(&m_awaitingSave, 0);

			// another program has the clipboard open: try again shortly, the answer keeps its place
			ULONGLONG now = ::GetTickCount64();
			if (m_writeStarted == 0)
			{
				m_writeStarted = now;
			}
			if (now - m_writeStarted < CLIPBOARD_WAIT_MS)
			{
				m_clipQueue.push_front(id);
				SetTimer(TIMER_PUMP_CLIPBOARD, CLIPBOARD_RETRY_MS, NULL);
				return;
			}
			m_writeStarted = 0;

			run->info.clipboardStatus = _T("Pano kilitli olduğu için panoya yazılamadı");
			Notify(NotifyKind::Error, _T("Pano kullanılamıyor"), _T("Model cevap verdi ama panoya yazılamadı. ") + ClipboardBusyText(), id);
			RunChanged(id);
			continue;
		}

		m_writeStarted = 0;
		m_savingRunId = id;
		m_lastClipWrite = ::GetTickCount64();
		run->info.clipboardStatus = _T("Panoya kopyalandı");
		SetTimer(TIMER_SAVE_FALLBACK, SAVE_FALLBACK_MS, NULL);
		RunChanged(id);
	}
}

// The answer on the clipboard was saved by Ditto (saved) or the wait timed out.
void CAiAssistant::FinishSave(bool saved)
{
	int id = m_savingRunId;
	m_savingRunId = 0;

	RunState *run = FindRun(id);
	if (run != nullptr)
	{
		AiRunInfo &info = run->info;
		info.clipboardStatus = saved ? _T("Panoya kopyalandı ve Ditto'ya kaydedildi") : _T("Panoya kopyalandı, Ditto geçmişine kaydı doğrulanamadı");

		// another answer is waiting and takes the clipboard right after this one
		bool replaced = !m_clipQueue.empty();
		CString title = replaced ? _T("Cevap hazır - panoya sıradaki cevap geçiyor")
			: saved ? _T("Cevap hazır - Ctrl+V ile yapıştırın") : _T("Cevap panoda - Ctrl+V ile yapıştırın");
		CString body = info.title + (saved ? _T(" · Ditto'ya kaydedildi") : _T(" · Ditto geçmişine kaydı doğrulanamadı"));
		if (info.warning.IsEmpty() == FALSE)
		{
			body += _T("\n") + info.warning;
		}
		if (m_config.notify.showPreview)
		{
			body += _T("\n\n") + OneLine(info.result, 160);
		}
		Notify(NotifyKind::Success, title, body, id);
		RunChanged(id);
	}

	PumpClipboard();
}

void CAiAssistant::OnClipsSaved(int clipId)
{
	if (m_hWnd != NULL && m_awaitingSave != 0)
	{
		::PostMessage(m_hWnd, WM_AI_CLIP_SAVED, (WPARAM)clipId, 0);
	}
}

LRESULT CAiAssistant::OnAiClipSaved(WPARAM wParam, LPARAM lParam)
{
	if (InterlockedExchange(&m_awaitingSave, 0) == 0)
	{
		return 0;
	}

	KillTimer(TIMER_SAVE_FALLBACK);
	FinishSave(true);
	return 0;
}

LRESULT CAiAssistant::OnToastClicked(WPARAM wParam, LPARAM lParam)
{
	// lParam: the toast had the "click for details" line, wParam: its key = run id
	if (lParam != 0 && !m_inSettings)
	{
		ShowRunDetail((int)wParam);
	}
	return 0;
}

// ---------------------------------------------------------------- runs

CAiAssistant::RunState *CAiAssistant::FindRun(int id)
{
	for (RunState &run : m_runs)
	{
		if (run.info.id == id)
		{
			return &run;
		}
	}
	return nullptr;
}

std::vector<AiRunInfo> CAiAssistant::RunList() const
{
	std::vector<AiRunInfo> list;
	for (auto it = m_runs.rbegin(); it != m_runs.rend(); ++it)
	{
		list.push_back(it->info);
	}
	return list;
}

void CAiAssistant::ShowRunDetail(int id)
{
	RunState *run = FindRun(id);
	if (run != nullptr)
	{
		m_detail.Show(run->info, this);
	}
}

void CAiAssistant::RunChanged(int id)
{
	RunState *run = FindRun(id);
	if (run != nullptr)
	{
		m_detail.Update(run->info);
	}
	PruneRuns();
	if (m_runsDlg.m_hWnd != NULL)
	{
		m_runsDlg.Update(RunList());
	}
}

// Keeps the newest MAX_RECENT_RUNS requests and every request that is still running
// or whose answer has not reached the clipboard yet.
void CAiAssistant::PruneRuns()
{
	int kept = 0;
	for (int i = (int)m_runs.size() - 1; i >= 0; i--)
	{
		const AiRunInfo &info = m_runs[i].info;
		bool pending = info.state == AiRunInfo::State::Running || info.id == m_savingRunId ||
			std::find(m_clipQueue.begin(), m_clipQueue.end(), info.id) != m_clipQueue.end();
		if (kept < MAX_RECENT_RUNS || pending)
		{
			kept++;
		}
		else
		{
			m_runs.erase(m_runs.begin() + i);
		}
	}
}

// ---------------------------------------------------------------- helpers

bool CAiAssistant::WriteClipboardText(const CString &text)
{
	size_t bytes = (text.GetLength() + 1) * sizeof(WCHAR);
	HGLOBAL hMem = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
	if (hMem == NULL)
	{
		return false;
	}
	memcpy(::GlobalLock(hMem), (LPCWSTR)text, bytes);
	::GlobalUnlock(hMem);

	// one attempt only: PumpClipboard retries with a timer so Ditto never freezes while waiting
	if (!::OpenClipboard(m_hWnd))
	{
		::GlobalFree(hMem);
		return false;
	}
	::EmptyClipboard();
	bool ok = ::SetClipboardData(CF_UNICODETEXT, hMem) != NULL;
	::CloseClipboard();
	if (!ok)
	{
		::GlobalFree(hMem);
	}
	return ok;
}

void CAiAssistant::Notify(NotifyKind kind, const CString &title, const CString &body, int runId)
{
	// each request has its own toast (key = run id), other notices share key 0
	const AiNotifySettings &n = m_config.notify;
	bool clickHint = runId != 0;
	switch (kind)
	{
	case NotifyKind::Progress:
		if (n.showProgress) ShowToast(n, AiToastKind::Info, title, body, true, runId, clickHint);
		break;
	case NotifyKind::Success:
		if (n.showSuccess) ShowToast(n, AiToastKind::Success, title, body, false, runId, clickHint);
		else m_toasts.Hide(runId, true);	// remove the "working" toast
		if (n.sound) ::MessageBeep(MB_ICONASTERISK);
		break;
	case NotifyKind::Error:
		if (n.showErrors) ShowToast(n, AiToastKind::Error, title, body, false, runId, clickHint);
		else m_toasts.Hide(runId, true);
		if (n.sound) ::MessageBeep(MB_ICONHAND);
		break;
	}
}

void CAiAssistant::ShowToast(const AiNotifySettings &settings, AiToastKind kind, const CString &title, const CString &body, bool sticky, int key, bool clickHint)
{
	UINT timeout = sticky ? 0 : (UINT)max(1, settings.durationSec) * 1000;
	if (kind == AiToastKind::Error && !sticky)
	{
		timeout = max(timeout, 8000u);	// errors need time to read
	}
	m_toasts.Show(key, title, body, kind, timeout, settings.position, clickHint);
}
