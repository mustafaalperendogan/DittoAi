#pragma once

#include <deque>
#include <vector>
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
//
// Requests run in parallel, each on its own thread with its own toast. Answers are put
// on the clipboard one at a time, in the order they arrive.

#define WM_AI_DOUBLE_COPY	(WM_USER + 300)
#define WM_AI_RESULT		(WM_USER + 301)		// wParam = run id, lParam = AiCallResult*
#define WM_AI_CLIP_SAVED	(WM_USER + 302)
// WM_AI_PICKED (WM_USER + 303) is defined in AiPickerWnd.h

// Command id for the menu entry that opens the AI settings.
#define ID_AI_SETTINGS		39001
// Command id for the quick paste right click entry that sends the selected clips to the AI.
#define ID_AI_SEND_CLIP		39002
// Command id for the menu entry that lists the running and recent AI requests.
#define ID_AI_REQUESTS		39003

class CAiAssistant : public CWnd
{
public:
	static CAiAssistant &Instance();

	void Start();
	void Stop();
	void ShowSettings(int startPage = 0);
	void ShowRequests();

	// Adds the AI settings and requests entries above the item beforeId (at the top if
	// the menu has no such item). Does nothing if they are already there.
	static void AddMenuItems(CMenu *pMenu, UINT beforeId);

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

	struct RunState
	{
		AiRunInfo info;			// shown in the requests and details windows
		AiModelConfig model;	// for the token usage record
	};

	void ApplySettings();
	void StartHook();
	void StopHook();

	bool ReadClipboard(ClipContent &content, CString &error, bool &locked);	// locked: another program has it open
	void ShowPicker();
	void Run(const CString &title, const CString &instruction, const AiModelConfig &model);
	bool WriteClipboardText(const CString &text);

	RunState *FindRun(int id);
	std::vector<AiRunInfo> RunList() const;	// newest first
	void ShowRunDetail(int id);
	void RunChanged(int id);		// refreshes the windows that show runs, drops old ones
	void PruneRuns();
	void PumpClipboard();
	void FinishSave(bool saved);

	enum class NotifyKind { Progress, Success, Error };
	// runId != 0: the toast belongs to that request, clicking it opens the request details
	void Notify(NotifyKind kind, const CString &title, const CString &body, int runId = 0);
	void ShowToast(const AiNotifySettings &settings, AiToastKind kind, const CString &title, const CString &body, bool sticky, int key = 0, bool clickHint = false);

	static DWORD WINAPI HookThreadProc(LPVOID param);
	static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
	static UINT RequestThreadProc(LPVOID param);

	AiConfig m_config;
	CAiToastStack m_toasts;
	CAiPickerWnd m_picker;
	ClipContent m_clip;
	HWND m_targetWnd;
	bool m_inSettings;
	bool m_readPending;				// a double copy waits to read the clipboard, do not overwrite it now
	ULONGLONG m_readStarted;		// tick of the first read attempt that found the clipboard locked, 0 = none
	std::vector<RunState> m_runs;	// every running request and the last finished ones, oldest first
	int m_runCounter;
	std::deque<int> m_clipQueue;	// finished runs whose answers wait for the clipboard
	int m_savingRunId;				// run whose answer is on the clipboard, waiting for Ditto to save it
	ULONGLONG m_lastClipWrite;		// tick of the last answer written to the clipboard
	ULONGLONG m_writeStarted;		// tick of the first write attempt that found the clipboard locked, 0 = none
	volatile LONG m_awaitingSave;
	CAiDetailDlg m_detail;
	CAiRunsDlg m_runsDlg;

	HANDLE m_hookThread;
	DWORD m_hookThreadId;
};
