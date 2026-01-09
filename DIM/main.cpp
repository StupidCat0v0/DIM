#define _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
#include <windows.h>
#include <commctrl.h>
#include <iostream>
#include <thread>
#include <string>
#include <chrono>
#include <iomanip>
#include <locale>
#include <codecvt>
#include <mutex>
#include <shlobj.h>
#include <atomic>
#include <tlhelp32.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "/SUBSYSTEM:WINDOWS /ENTRY:WinMainCRTStartup")

// 全局常量定义
#define WM_TRAYICON (WM_USER + 1)    // 托盘图标消息
#define ID_TRAY_EXIT 1001            // 退出菜单项ID
#define ID_TRAY_RESTORE 1002         // 恢复图标菜单项ID
#define ID_TRAY_HIDE 1003            // 隐藏图标菜单项ID
#define ID_TRAY_SETTINGS 1004        // 设置菜单项ID
#define ID_TRAY_AUTO_START 1005      // 开机自启动菜单项ID
#define ID_TRAY_RESTART_EXPLORER 1006 // 重启资源管理器菜单项ID

// CD时间常量（毫秒）
const DWORD CD_INTERVAL = 500;      // 3秒冷却时间

// 全局变量
HINSTANCE g_hInstance = NULL;
HWND g_hWnd = NULL;
HHOOK g_hMouseHook = NULL;
NOTIFYICONDATA g_nid = { 0 };
bool g_bIconHidden = true;          // 图标是否已隐藏
std::mutex g_mtxClick;               // 点击检测互斥锁
int g_nClickCount = 0;               // 点击计数
DWORD g_dwLastClickTime = 0;         // 上一次点击时间
const DWORD TRIPLE_CLICK_INTERVAL = 300; // 三击检测间隔
DWORD g_dwLastOperationTime = 0;     // 上一次执行隐藏/显示操作的时间
std::mutex g_mtxCD;                 // CD时间互斥锁
bool g_bAutoStart = false;          // 开机自启动状态

// 用于管理自动隐藏线程的变量
std::atomic<bool> g_bAutoHideThreadActive{ false };  // 是否有自动隐藏线程在运行
std::atomic<bool> g_bCancelAutoHide{ false };        // 是否取消自动隐藏

// 重启资源管理器函数
void CreateTrayIcon();
void DeleteTrayIcon();

// 获取当前程序路径
std::wstring GetExecutablePath() {
	WCHAR path[MAX_PATH];
	GetModuleFileNameW(NULL, path, MAX_PATH);
	return std::wstring(path);
}

bool RestartExplorer() {
	//// 查找并终止explorer.exe进程
	//PROCESSENTRY32 pe32;
	//pe32.dwSize = sizeof(PROCESSENTRY32);
	//HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	//
	//if (hSnapshot == INVALID_HANDLE_VALUE) {
	//	return false;
	//}
	//
	//bool explorerFound = false;
	//if (Process32First(hSnapshot, &pe32)) {
	//	do {
	//		if (wcscmp(pe32.szExeFile, L"explorer.exe") == 0) {
	//			HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, pe32.th32ProcessID);
	//			if (hProcess != NULL) {
	//				TerminateProcess(hProcess, 0);
	//				CloseHandle(hProcess);
	//				explorerFound = true;
	//			}
	//		}
	//	} while (Process32Next(hSnapshot, &pe32));
	//}
	//CloseHandle(hSnapshot);


	// 获取当前程序路径，用于重启
	std::wstring currentExePath = GetExecutablePath();

	// 重启当前程序
	ShellExecuteW(NULL, L"open", currentExePath.c_str(), NULL, NULL, SW_SHOWNORMAL);

	PostQuitMessage(0);
	return true;
}

// 检查是否已设置开机自启动
bool IsAutoStartEnabled() {
	HKEY hKey;
	LONG result = RegOpenKeyEx(HKEY_CURRENT_USER,
		L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
		0, KEY_READ, &hKey);

	if (result == ERROR_SUCCESS) {
		WCHAR value[1024];
		DWORD size = sizeof(value);
		result = RegGetValue(hKey, NULL, L"DesktopIconManagement", RRF_RT_REG_SZ, NULL, value, &size);
		RegCloseKey(hKey);

		return (result == ERROR_SUCCESS);
	}
	return false;
}

// 设置/取消开机自启动
bool SetAutoStart(bool enable) {
	HKEY hKey;
	LONG result = RegOpenKeyEx(HKEY_CURRENT_USER,
		L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
		0, KEY_WRITE, &hKey);

	if (result == ERROR_SUCCESS) {
		if (enable) {
			// 获取当前程序路径
			WCHAR exePath[MAX_PATH] = GetExecutablePath();

			result = RegSetValueEx(hKey, L"DesktopIconManagement", 0, REG_SZ,
				(BYTE*)exePath, (wcslen(exePath) + 1) * sizeof(WCHAR));
		}
		else {
			result = RegDeleteValue(hKey, L"DesktopIconManagement");
		}
		RegCloseKey(hKey);
		return (result == ERROR_SUCCESS);
	}
	return false;
}

// 获取桌面图标窗口句柄
HWND GetDesktopListView() {
	HWND progman = FindWindow(L"Progman", NULL);
	if (progman) {
		HWND desktop = FindWindowEx(progman, 0, L"SHELLDLL_DefView", NULL);
		if (desktop) {
			return FindWindowEx(desktop, 0, L"SysListView32", NULL);
		}
		// 兼容Windows 10/11的新桌面布局
		SendMessage(progman, 0x052C, 0, 0);
		desktop = FindWindowEx(progman, 0, L"SHELLDLL_DefView", NULL);
		if (desktop) {
			return FindWindowEx(desktop, 0, L"SysListView32", NULL);
		}
	}
	return NULL;
}

// 检查是否在CD冷却期内
bool IsInCDInterval() {
	std::lock_guard<std::mutex> lock(g_mtxCD);
	DWORD dwCurrentTime = GetTickCount();
	return (dwCurrentTime - g_dwLastOperationTime) < CD_INTERVAL;
}

// 更新最后操作时间（仅在真正执行操作时调用）
void UpdateLastOperationTime() {
	std::lock_guard<std::mutex> lock(g_mtxCD);
	g_dwLastOperationTime = GetTickCount();
}

// 隐藏桌面图标
void FadeOutDesktopIcons() {
	// 检查CD冷却
	if (IsInCDInterval()) return;

	HWND desktopListView = GetDesktopListView();
	if (!desktopListView) return;

	// 获取图标数量
	int itemCount = (int)SendMessage(desktopListView, LVM_GETITEMCOUNT, 0, 0);
	if (itemCount <= 0) return;
	// 最终隐藏所有图标
	ShowWindow(desktopListView, SW_HIDE);
	g_bIconHidden = true;

	// 取消任何正在运行的自动隐藏线程
	g_bCancelAutoHide = true;

	// 只有真正执行了隐藏操作才更新时间
	UpdateLastOperationTime();
}

// 恢复桌面图标
void RestoreDesktopIcons() {
	// 检查CD冷却
	if (IsInCDInterval()) return;

	HWND desktopListView = GetDesktopListView();
	if (desktopListView) {
		ShowWindow(desktopListView, SW_SHOW);
		g_bIconHidden = false;

		// 取消任何正在运行的自动隐藏线程
		g_bCancelAutoHide = true;

		// 只有真正执行了恢复操作才更新时间
		UpdateLastOperationTime();

		// 创建线程，等待1分钟后自动隐藏图标
		// 首先检查是否已经有自动隐藏线程在运行，如果有则取消它
		if (g_bAutoHideThreadActive.load()) {
			g_bCancelAutoHide = true;
		}

		// 重置取消标志
		g_bCancelAutoHide = false;
		g_bAutoHideThreadActive = true;

		std::thread([]() {
			// 等待1分钟
			auto start_time = std::chrono::steady_clock::now();
			auto end_time = start_time + std::chrono::minutes(1);

			while (std::chrono::steady_clock::now() < end_time && !g_bCancelAutoHide.load()) {
				std::this_thread::sleep_for(std::chrono::milliseconds(100)); // 每100ms检查一次
			}

			// 检查是否被取消
			if (g_bCancelAutoHide.load()) {
				g_bAutoHideThreadActive = false;
				return; // 线程被取消，直接退出
			}

			// 检查是否仍在CD期内，如果不在则执行隐藏
			if (!IsInCDInterval()) {
				FadeOutDesktopIcons();
			}

			g_bAutoHideThreadActive = false; // 重置线程活动标志
			}).detach(); // 分离线程，自动回收资源
	}
}

// 获取窗口类名
std::string GetWindowClassName(HWND hWnd) {
	if (hWnd == NULL) return "NULL";

	wchar_t wClassName[256] = { 0 };
	GetClassNameW(hWnd, wClassName, 256);

	std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
	return conv.to_bytes(wClassName);
}
// 鼠标钩子处理函数
LRESULT CALLBACK MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
	if (nCode >= 0) {
		MSLLHOOKSTRUCT* pMouseStruct = (MSLLHOOKSTRUCT*)lParam;

		// 检测桌面区域的左键三击
		if (wParam == WM_LBUTTONDOWN) {
			// 如果在CD期内，直接返回
			if (IsInCDInterval()) {
				return CallNextHookEx(g_hMouseHook, nCode, wParam, lParam);
			}

			HWND hForegroundWnd = GetForegroundWindow();
			std::string className = GetWindowClassName(hForegroundWnd);

			// 检测是否点击在桌面区域
			if (className == "Progman" || className == "WorkerW") {
				std::lock_guard<std::mutex> lock(g_mtxClick);
				DWORD dwCurrentTime = GetTickCount();

				// 三击检测逻辑
				if (dwCurrentTime - g_dwLastClickTime <= TRIPLE_CLICK_INTERVAL) {
					g_nClickCount++;
					if (g_nClickCount >= 3) {
						// 切换图标显示状态
						if (g_bIconHidden) {
							RestoreDesktopIcons();
						}
						else {
							FadeOutDesktopIcons();
						}
						g_nClickCount = 0; // 重置计数
					}
				}
				else {
					g_nClickCount = 1; // 超过间隔，重置为第一次点击
				}
				g_dwLastClickTime = dwCurrentTime;
			}
		}
	}

	// 传递钩子链
	return CallNextHookEx(g_hMouseHook, nCode, wParam, lParam);
}

// 安装鼠标钩子
bool InstallMouseHook() {
	g_hMouseHook = SetWindowsHookEx(WH_MOUSE_LL, MouseHookProc, g_hInstance, 0);
	if (g_hMouseHook == NULL) {
		return false;
	}
	return true;
}

// 卸载鼠标钩子
void UninstallMouseHook() {
	if (g_hMouseHook != NULL) {
		UnhookWindowsHookEx(g_hMouseHook);
		g_hMouseHook = NULL;
	}
}

// 创建托盘图标
void CreateTrayIcon() {
	ZeroMemory(&g_nid, sizeof(NOTIFYICONDATA));
	g_nid.cbSize = sizeof(NOTIFYICONDATA);
	g_nid.hWnd = g_hWnd;
	g_nid.uID = 1;
	g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	g_nid.uCallbackMessage = WM_TRAYICON;

	// 设置托盘提示文本（添加CD提示）
	wcscpy_s(g_nid.szTip, L"桌面图标隐藏工具\n三击桌面切换显示/隐藏\n操作后有3秒冷却时间");

	// 使用默认应用程序图标
	g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);

	// 添加托盘图标
	Shell_NotifyIcon(NIM_ADD, &g_nid);
}

// 删除托盘图标
void DeleteTrayIcon() {
	Shell_NotifyIcon(NIM_DELETE, &g_nid);
	if (g_nid.hIcon) {
		DestroyIcon(g_nid.hIcon);
		g_nid.hIcon = NULL;
	}
}

// 创建托盘右键菜单
void ShowTrayMenu() {
	POINT pt;
	GetCursorPos(&pt);

	HMENU hMenu = CreatePopupMenu();

	// 添加菜单项（根据CD状态灰显）
	UINT restoreFlags = MF_STRING;
	UINT hideFlags = MF_STRING;
	UINT autoStartFlags = MF_STRING;

	if (IsInCDInterval()) {
		restoreFlags |= MF_GRAYED;
		hideFlags |= MF_GRAYED;
	}

	if (g_bIconHidden) {
		AppendMenu(hMenu, restoreFlags, ID_TRAY_RESTORE, L"恢复桌面图标");
	}
	else {
		AppendMenu(hMenu, hideFlags, ID_TRAY_HIDE, L"隐藏桌面图标");
	}

	// 添加自启动菜单项
	if (g_bAutoStart) {
		AppendMenu(hMenu, autoStartFlags, ID_TRAY_AUTO_START, L"取消开机自启动");
	}
	else {
		AppendMenu(hMenu, autoStartFlags, ID_TRAY_AUTO_START, L"设置开机自启动");
	}

	// 添加重启资源管理器菜单项
	AppendMenu(hMenu, MF_STRING, ID_TRAY_RESTART_EXPLORER, L"重启资源管理器");
	AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
	AppendMenu(hMenu, MF_STRING, ID_TRAY_EXIT, L"退出程序");

	// 设置菜单默认项
	SetMenuDefaultItem(hMenu, ID_TRAY_EXIT, FALSE);

	// 显示菜单并跟踪选择
	SetForegroundWindow(g_hWnd);
	TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, g_hWnd, NULL);
	PostMessage(g_hWnd, WM_NULL, 0, 0);

	// 销毁菜单
	DestroyMenu(hMenu);
}

// 窗口过程函数
LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	int wait_count = 0;
	switch (uMsg) {
	case WM_TRAYICON: {
		if (lParam == WM_RBUTTONUP) {
			// 右键点击托盘图标，显示菜单
			ShowTrayMenu();
		}
		else if (lParam == WM_LBUTTONDBLCLK) {
			// 左键双击托盘图标，切换图标状态（先检查CD）
			if (IsInCDInterval()) break;

			if (g_bIconHidden) {
				RestoreDesktopIcons();
			}
			else {
				FadeOutDesktopIcons();
			}
		}
		return 0;
	}

	case WM_COMMAND: {
		switch (LOWORD(wParam)) {
		case ID_TRAY_EXIT:
			// 退出程序
			PostQuitMessage(0);
			break;
		case ID_TRAY_RESTORE:
			// 恢复桌面图标（检查CD）
			if (!IsInCDInterval()) {
				RestoreDesktopIcons();
			}
			break;
		case ID_TRAY_HIDE:
			// 隐藏桌面图标（检查CD）
			if (!IsInCDInterval()) {
				FadeOutDesktopIcons();
			}
			break;
		case ID_TRAY_AUTO_START:
			// 切换开机自启动
			g_bAutoStart = !g_bAutoStart;
			if (SetAutoStart(g_bAutoStart)) {
				MessageBox(NULL,
					g_bAutoStart ? L"已设置开机自启动" : L"已取消开机自启动",
					L"设置",
					MB_ICONINFORMATION | MB_OK);
			}
			else {
				MessageBox(NULL, L"设置开机自启动失败", L"错误", MB_ICONERROR | MB_OK);
			}
			break;
		case ID_TRAY_RESTART_EXPLORER:
			// 重启资源管理器
			if (!RestartExplorer()) {
				MessageBox(NULL, L"重启资源管理器失败", L"错误", MB_ICONERROR | MB_OK);
			}
			break;
		}
		return 0;
	}

	case WM_SIZE: {
		// 窗口最小化时隐藏主窗口
		if (wParam == SIZE_MINIMIZED) {
			ShowWindow(hWnd, SW_HIDE);
		}
		return 0;
	}

	case WM_DESTROY:
		// 在程序退出前取消任何正在运行的自动隐藏线程
		g_bCancelAutoHide = true;

		// 等待自动隐藏线程结束（最多等待1秒）
		while (g_bAutoHideThreadActive.load() && wait_count < 10) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			wait_count++;
		}

		// 程序退出清理
		DeleteTrayIcon();
		UninstallMouseHook();
		PostQuitMessage(0);
		return 0;

	default:
		return DefWindowProc(hWnd, uMsg, wParam, lParam);
	}
}

// 注册窗口类
ATOM RegisterWindowClass(HINSTANCE hInstance) {
	WNDCLASSEX wc = { 0 };
	wc.cbSize = sizeof(WNDCLASSEX);
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hInstance;
	wc.lpszClassName = L"DesktopIconManagement";
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

	return RegisterClassEx(&wc);
}
bool TerminateExistingProcess(const wchar_t* processName) {
	DWORD currentPID = GetCurrentProcessId();
	HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (hSnapshot == INVALID_HANDLE_VALUE) {
		return false;
	}

	PROCESSENTRY32W pe32;
	pe32.dwSize = sizeof(PROCESSENTRY32W);

	bool foundAndTerminated = false;

	if (Process32FirstW(hSnapshot, &pe32)) {
		do {
			// 忽略当前进程
			if (pe32.th32ProcessID == currentPID)
				continue;

			// 比较进程名（不区分大小写）
			if (_wcsicmp(pe32.szExeFile, processName) == 0) {
				HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, pe32.th32ProcessID);
				if (hProcess != NULL) {
					if (TerminateProcess(hProcess, 0)) {
						std::wcout << L"已终止进程: " << processName
							<< L" (PID: " << pe32.th32ProcessID << L")\n";
						foundAndTerminated = true;
					}
					CloseHandle(hProcess);
				}
			}
		} while (Process32NextW(hSnapshot, &pe32));
	}

	CloseHandle(hSnapshot);
	return foundAndTerminated;
}

// WinMain入口函数
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
	// 检查是否已运行
	if (TerminateExistingProcess(L"DesktopIconManagement")) {
		MessageBox(NULL, L"程序已在运行中！", L"提示", MB_ICONINFORMATION | MB_OK);
		return 1;
	}

	g_hInstance = hInstance;

	// 初始化通用控件
	INITCOMMONCONTROLSEX icex;
	icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
	icex.dwICC = ICC_WIN95_CLASSES;
	InitCommonControlsEx(&icex);

	// 注册窗口类
	if (!RegisterWindowClass(hInstance)) {
		MessageBox(NULL, L"窗口类注册失败！", L"错误", MB_ICONERROR | MB_OK);
		return 1;
	}

	// 创建隐藏的主窗口
	g_hWnd = CreateWindowEx(
		0,
		L"DesktopIconManagement",
		L"桌面图标隐藏工具",
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT,
		400, 300,
		NULL,
		NULL,
		hInstance,
		NULL
	);

	if (!g_hWnd) {
		MessageBox(NULL, L"窗口创建失败！", L"错误", MB_ICONERROR | MB_OK);
		return 1;
	}

	// 检查当前自启动状态
	g_bAutoStart = IsAutoStartEnabled();

	// 创建托盘图标
	CreateTrayIcon();

	// 安装鼠标钩子
	if (!InstallMouseHook()) {
		MessageBox(NULL, L"鼠标钩子安装失败！", L"错误", MB_ICONERROR | MB_OK);
		DeleteTrayIcon();
		DestroyWindow(g_hWnd);
		return 1;
	}

	// 隐藏主窗口（只显示托盘图标）
	ShowWindow(g_hWnd, SW_HIDE);
	UpdateWindow(g_hWnd);
	FadeOutDesktopIcons();

	// 消息循环
	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	// 清理资源
	UninstallMouseHook();
	DeleteTrayIcon();
	DestroyWindow(g_hWnd);
	UnregisterClass(L"DesktopIconManagement", hInstance);

	return (int)msg.wParam;
}