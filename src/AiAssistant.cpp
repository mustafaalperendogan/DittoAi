#include "stdafx.h"
#include "AiAssistant.h"
#include "AiDialogs.h"
#include "AiRag.h"
#include "AiUsage.h"
#include "Misc.h"

#include <memory>
#include <wincrypt.h>

namespace
{
	const UINT_PTR TIMER_READ_CLIPBOARD = 1;
	const UINT_PTR TIMER_SAVE_FALLBACK = 2;

	const UINT READ_CLIPBOARD_DELAY_MS = 200;	// let the source app finish its copy
	const UINT SAVE_FALLBACK_MS = 5000;			// how long to wait for Ditto to save the answer

	const int MAX_CLIP_CHARS = 400000;
	const int MAX_IMAGE_EDGE = 1568;			// larger images are scaled down before sending

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
	: m_targetWnd(NULL), m_busy(false), m_inSettings(false), m_awaitingSave(0), m_runCounter(0), m_toastForRun(false), m_hookThread(NULL), m_hookThreadId(0)
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
	m_toast.SetClickTarget(m_hWnd);

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
	if (m_toast.m_hWnd != NULL)
	{
		m_toast.DestroyWindow();
	}
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

	if (m_busy)
	{
		Notify(NotifyKind::Info, _T("Yapay zeka hâlâ çalışıyor"), _T("Önceki istek bitince tekrar deneyin."));
		return 0;
	}

	m_targetWnd = ::GetForegroundWindow();

	// the second Ctrl+C is still being handled by the source app, give it a moment
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

	if (m_busy)
	{
		Notify(NotifyKind::Info, _T("Yapay zeka hâlâ çalışıyor"), _T("Önceki istek bitince tekrar deneyin."));
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
		if (!ReadClipboard(content, error))
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
			m_run.clipboardStatus = _T("Panoya kopyalandı, Ditto geçmişine kaydı doğrulanamadı");
			m_detail.Update(m_run);
			Notify(NotifyKind::Success, _T("Cevap panoda - Ctrl+V ile yapıştırın"),
				m_lastTitle + _T(" · Ditto geçmişine kaydı doğrulanamadı") + (m_config.notify.showPreview ? _T("\n\n") + OneLine(m_lastResult, 160) : CString()), true);
		}
		return;
	}

	CWnd::OnTimer(nIDEvent);
}

bool CAiAssistant::ReadClipboard(ClipContent &content, CString &error)
{
	bool opened = false;
	for (int attempt = 0; attempt < 10 && !opened; attempt++)
	{
		opened = ::OpenClipboard(m_hWnd) != FALSE;
		if (!opened) Sleep(30);
	}
	if (!opened)
	{
		error = _T("Pano başka bir uygulama tarafından kilitli.");
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

	AiJob *job = new AiJob();
	job->notifyWnd = m_hWnd;
	job->input.model = model;
	job->input.apiKey = apiKey;
	job->input.instruction = instruction;
	job->input.text = m_clip.isImage ? CString() : m_clip.text;
	job->input.image = m_clip.image;

	m_busy = true;
	m_runModel = model;
	m_lastTitle = title;
	m_lastWarning.Empty();
	InterlockedExchange(&m_awaitingSave, 0);
	KillTimer(TIMER_SAVE_FALLBACK);

	CString provider = AiModelConfig::ProviderName(model.provider);
	int paren = provider.Find(_T(" ("));
	if (paren > 0)
	{
		provider = provider.Left(paren);
	}
	m_run = AiRunInfo();
	m_run.valid = true;
	m_run.id = ++m_runCounter;
	m_run.title = title;
	m_run.instruction = instruction;
	m_run.modelName = model.name;
	m_run.modelDetail = provider + _T(" · ") + model.model;
	m_run.isImage = m_clip.isImage;
	m_run.imageWidth = m_clip.image.width;
	m_run.imageHeight = m_clip.image.height;
	m_run.inputText = m_clip.isImage ? CString() : m_clip.text;
	m_run.started = CTime::GetCurrentTime();
	m_run.startTick = ::GetTickCount64();

	CString what = m_clip.isImage ? StrF(_T("Görsel %d × %d"), m_clip.image.width, m_clip.image.height) : OneLine(m_clip.text, 120);
	Notify(NotifyKind::Progress, model.name + _T(" çalışıyor..."), title + _T("\n") + what, true);

	if (AfxBeginThread(RequestThreadProc, job) == NULL)
	{
		delete job;
		m_busy = false;
		m_run.state = AiRunInfo::State::Error;
		m_run.error = _T("İstek başlatılamadı.");
		Notify(NotifyKind::Error, _T("İstek başlatılamadı"), CString(), true);
	}
}

UINT CAiAssistant::RequestThreadProc(LPVOID param)
{
	std::unique_ptr<AiJob> job((AiJob *)param);

	if (job->input.model.ragEnabled)
	{
		job->input.ragContext = AiRagRetrieve(job->input.model, job->input.instruction + _T("\n") + job->input.text);
	}

	AiCallResult *result = new AiCallResult(AiCallModel(job->input));
	if (!::PostMessage(job->notifyWnd, WM_AI_RESULT, 0, (LPARAM)result))
	{
		delete result;	// Ditto is closing
	}
	return 0;
}

LRESULT CAiAssistant::OnAiResult(WPARAM wParam, LPARAM lParam)
{
	std::unique_ptr<AiCallResult> result((AiCallResult *)lParam);
	m_busy = false;

	// a refused or empty answer still used tokens if the provider reported them
	if (result->ok || result->usageKnown)
	{
		AiUsageRecord(m_runModel, result->usageKnown, result->inputTokens, result->outputTokens);
	}

	m_run.durationMs = ::GetTickCount64() - m_run.startTick;
	m_run.usageKnown = result->usageKnown;
	m_run.inputTokens = result->inputTokens;
	m_run.outputTokens = result->outputTokens;

	if (!result->ok)
	{
		m_run.state = AiRunInfo::State::Error;
		m_run.error = result->error;
		m_detail.Update(m_run);
		Notify(NotifyKind::Error, _T("Model cevap veremedi"), result->error, true);
		return 0;
	}

	CString text = result->text;
	text.Replace(_T("\r\n"), _T("\n"));
	text.Replace(_T("\n"), _T("\r\n"));
	m_lastResult = text;
	m_lastWarning = result->warning;

	m_run.state = AiRunInfo::State::Success;
	m_run.result = text;
	m_run.warning = result->warning;

	// waiting for Ditto's save thread to report the new clip
	InterlockedExchange(&m_awaitingSave, 1);

	if (!WriteClipboardText(text))
	{
		InterlockedExchange(&m_awaitingSave, 0);
		m_run.clipboardStatus = _T("Pano kilitli olduğu için panoya yazılamadı");
		m_detail.Update(m_run);
		Notify(NotifyKind::Error, _T("Pano kullanılamıyor"), _T("Model cevap verdi ama pano başka bir uygulama tarafından kilitli."), true);
		return 0;
	}

	m_run.clipboardStatus = _T("Panoya kopyalandı");
	m_detail.Update(m_run);
	SetTimer(TIMER_SAVE_FALLBACK, SAVE_FALLBACK_MS, NULL);
	return 0;
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

	m_run.clipboardStatus = _T("Panoya kopyalandı ve Ditto'ya kaydedildi");
	m_detail.Update(m_run);

	CString body = m_lastTitle + _T(" · Ditto'ya kaydedildi");
	if (m_lastWarning.IsEmpty() == FALSE)
	{
		body += _T("\n") + m_lastWarning;
	}
	if (m_config.notify.showPreview)
	{
		body += _T("\n\n") + OneLine(m_lastResult, 160);
	}
	Notify(NotifyKind::Success, _T("Cevap hazır - Ctrl+V ile yapıştırın"), body, true);
	return 0;
}

LRESULT CAiAssistant::OnToastClicked(WPARAM wParam, LPARAM lParam)
{
	if (m_toastForRun && m_run.valid && !m_inSettings)
	{
		m_detail.Show(m_run, this);
	}
	return 0;
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

	for (int attempt = 0; attempt < 10; attempt++)
	{
		if (::OpenClipboard(m_hWnd))
		{
			::EmptyClipboard();
			bool ok = ::SetClipboardData(CF_UNICODETEXT, hMem) != NULL;
			::CloseClipboard();
			if (!ok)
			{
				::GlobalFree(hMem);
			}
			return ok;
		}
		Sleep(30);
	}

	::GlobalFree(hMem);
	return false;
}

void CAiAssistant::Notify(NotifyKind kind, const CString &title, const CString &body, bool forRun)
{
	const AiNotifySettings &n = m_config.notify;
	m_toastForRun = forRun;
	switch (kind)
	{
	case NotifyKind::Progress:
		if (n.showProgress) ShowToast(n, AiToastKind::Info, title, body, true, forRun);
		break;
	case NotifyKind::Info:
		if (n.showProgress) ShowToast(n, AiToastKind::Info, title, body, false, forRun);
		break;
	case NotifyKind::Success:
		if (n.showSuccess) ShowToast(n, AiToastKind::Success, title, body, false, forRun);
		else m_toast.HideToast(true);	// remove the "working" toast
		if (n.sound) ::MessageBeep(MB_ICONASTERISK);
		break;
	case NotifyKind::Error:
		if (n.showErrors) ShowToast(n, AiToastKind::Error, title, body, false, forRun);
		else m_toast.HideToast(true);
		if (n.sound) ::MessageBeep(MB_ICONHAND);
		break;
	}
}

void CAiAssistant::ShowToast(const AiNotifySettings &settings, AiToastKind kind, const CString &title, const CString &body, bool sticky, bool clickHint)
{
	UINT timeout = sticky ? 0 : (UINT)max(1, settings.durationSec) * 1000;
	if (kind == AiToastKind::Error && !sticky)
	{
		timeout = max(timeout, 8000u);	// errors need time to read
	}
	m_toast.ShowToast(title, body, kind, timeout, settings.position, clickHint);
}
