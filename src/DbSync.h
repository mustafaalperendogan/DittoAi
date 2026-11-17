#pragma once

#include <set>
#include <utility>

// Cloud sync of the Ditto database.
//
// The whole history travels as one JSON package that is encrypted with the user's
// password (PBKDF2-SHA256 + AES-256-GCM) and stored on a REST API:
//   GET <url>  -> 200 + package + ETag, or 404 when the cloud is empty
//   PUT <url>  -> If-Match: <etag> (If-None-Match: * for the first upload); 412 = someone else uploaded first
//   Authorization: Bearer <api key>
// Every computer merges both ways; per clip the newest change wins. Sync state (clip id <-> global id,
// last synced hashes) lives in the DittoSync table of the same database.

const UINT WM_DB_SYNC_STATUS = WM_APP + 160;	// to the status window: sync started or finished
const UINT WM_DB_SYNC_TEST_DONE = WM_APP + 161;	// to the window that started a connection test; lParam = CString* (delete it)

struct DbSyncSettings
{
	bool enabled = false;
	CString url;
	CString apiKey;
	CString password;			// encrypts the package; must be the same on every computer
	bool autoSync = true;
	int intervalMinutes = 15;
	bool syncOnStart = true;
	bool includeRichFormats = false;	// RTF, HTML and images besides plain text
	int maxClipKB = 1024;				// formats bigger than this are not sent
	bool propagateDeletes = false;		// clips deleted here are deleted on the other computers too

	void Load();
	void Save() const;
	CString Validate() const;	// empty = ok, otherwise a message for the user
};

struct DbSyncResult
{
	bool ok = false;
	CString error;
	bool localChanged = false;	// the database was modified, the clip list needs a refresh
	int uploaded = 0;
	int downloaded = 0;
	int updated = 0;
	int deletedLocal = 0;
	int deletedRemote = 0;
	int remoteClips = 0;

	CString Summary() const;
};

// Clips that were checked and have nothing to send with the current settings (clip id, CRC).
struct DbSyncSkipCache
{
	bool includeRich = false;
	int maxClipKB = 0;
	std::set<std::pair<int, int>> clips;
};

// Blocking, run on worker threads.
DbSyncResult DbSyncRun(const DbSyncSettings &settings, const CString &dbPath, volatile LONG *pStop, DbSyncSkipCache &skipCache);
bool DbSyncTestConnection(const DbSyncSettings &settings, CString &message);

// Owns the sync worker thread. Hidden window: receives the results on the main thread.
class CDbSync : public CWnd
{
public:
	static CDbSync &Instance();

	void Start();
	void Stop();

	void SettingsChanged();				// settings were saved, reload them and reschedule
	void SyncNow();
	void StartTest(const DbSyncSettings &settings, HWND hNotify);	// result: WM_DB_SYNC_TEST_DONE, wParam = success

	void SetStatusWindow(HWND hWnd) { m_statusWnd = hWnd; }
	bool IsRunning() const { return m_running != 0; }
	static CString LastStatus();

protected:
	DECLARE_MESSAGE_MAP()
	afx_msg LRESULT OnSyncDone(WPARAM wParam, LPARAM lParam);

private:
	CDbSync();
	~CDbSync();

	static DWORD WINAPI ThreadProc(LPVOID param);
	void Run();
	DbSyncSettings GetSettings();

	CRITICAL_SECTION m_lock;
	DbSyncSettings m_settings;
	HANDLE m_thread = NULL;
	HANDLE m_stopEvent = NULL;
	HANDLE m_syncEvent = NULL;
	HANDLE m_reloadEvent = NULL;
	volatile LONG m_stop = 0;
	volatile LONG m_running = 0;
	HWND m_statusWnd = NULL;
	DbSyncSkipCache m_skipCache;		// used by the worker thread only
};
