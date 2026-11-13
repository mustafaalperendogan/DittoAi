#pragma once

#include "AiConfig.h"
#include "AiProviders.h"
#include "AiToastWnd.h"
#include "AiPickerWnd.h"
#include "AiDialogs.h"

// "Double Ctrl+C" AI assistant.
//
// Pressing Ctrl+C twice quickly opens a searchable list of actions next to the mouse.
// The copied text or image and the chosen instruction are sent to the model the
// action is routed to (any provider, optionally with RAG); the answer is put on the
// clipboard so Ditto saves it like any other copy, and Ditto's own toast tells the
// user it is ready to paste.

#define WM_AI_DOUBLE_COPY	(WM_USER + 300)
#define WM_AI_RESULT		(WM_USER + 301)
#define WM_AI_CLIP_SAVED	(WM_USER + 302)
// WM_AI_PICKED (WM_USER + 303) is defined in AiPickerWnd.h

// Command id for the tray menu entry that opens the AI settings.
#define ID_AI_SETTINGS		39001
// Command id for the quick paste right click entry that sends the selected clips to the AI.
#define ID_AI_SEND_CLIP		39002

class CAiAssistant : public CWnd
{
public:
	static CAiAssistant &Instance();

	void Start();
	void Stop();
	void ShowSettings(int startPage = 0);

	// Opens the action list for clips chosen in Ditto's own window (right click menu).
	// Text wins over image; targetWnd gets the focus back after an action is picked.
	void SendClip(const CString &text, Gdiplus::Bitmap *image, HWND targetWnd);

	// Called from Ditto's save thread after clips were written to the database.
	void OnClipsSaved(int clipId);

protected:
	CAiAssistant();
	virtual ~CAiAssistant();

	DECLARE_MESSAGE_MAP()
	afx_msg LRESULT OnDoubleCopy(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnPicked(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnAiResult(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnAiClipSaved(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnToastClicked(WPARAM wParam, LPARAM lParam);
	afx_msg void OnTimer(UINT_PTR nIDEvent);

	struct ClipContent
	{
		bool isImage = false;
		CString text;
		AiImage image;
	};

	void ApplySettings();
	void StartHook();
	void StopHook();

	bool ReadClipboard(ClipContent &content, CString &error);
	void ShowPicker();
	void Run(const CString &title, const CString &instruction, const AiModelConfig &model);
	bool WriteClipboardText(const CString &text);

	enum class NotifyKind { Progress, Success, Error, Info };
	// forRun: the toast belongs to the current request, clicking it opens the request details
	void Notify(NotifyKind kind, const CString &title, const CString &body, bool forRun = false);
	void ShowToast(const AiNotifySettings &settings, AiToastKind kind, const CString &title, const CString &body, bool sticky, bool clickHint = false);

	static DWORD WINAPI HookThreadProc(LPVOID param);
	static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
	static UINT RequestThreadProc(LPVOID param);

	AiConfig m_config;
	CAiToastWnd m_toast;
	CAiPickerWnd m_picker;
	ClipContent m_clip;
	HWND m_targetWnd;
	bool m_busy;
	bool m_inSettings;
	volatile LONG m_awaitingSave;
	AiModelConfig m_runModel;	// model of the running request, for the token usage record
	AiRunInfo m_run;			// last request, shown in the details window
	int m_runCounter;
	bool m_toastForRun;			// the visible toast belongs to m_run
	CAiDetailDlg m_detail;
	CString m_lastTitle;
	CString m_lastResult;
	CString m_lastWarning;

	HANDLE m_hookThread;
	DWORD m_hookThreadId;
};
