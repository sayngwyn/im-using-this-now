#include "pch.h"

#include "Settings.h"

namespace
{
	std::filesystem::path IniPath()
	{
		HMODULE self = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&IniPath), &self);
		wchar_t buf[MAX_PATH]{};
		GetModuleFileNameW(self, buf, MAX_PATH);
		return std::filesystem::path(buf).replace_extension(L".ini");
	}

	std::string ReadString(const wchar_t* a_section, const wchar_t* a_key, const std::string& a_default, const std::wstring& a_ini)
	{
		wchar_t buf[512]{};
		const auto len = GetPrivateProfileStringW(a_section, a_key, L"\x1", buf, 512, a_ini.c_str());
		if (len == 1 && buf[0] == L'\x1') {
			return a_default;
		}
		const int size = WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
		std::string out(static_cast<std::size_t>(size), '\0');
		WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(len), out.data(), size, nullptr, nullptr);
		return out;
	}

	float ReadFloat(const wchar_t* a_section, const wchar_t* a_key, float a_default, const std::wstring& a_ini)
	{
		wchar_t buf[64]{};
		GetPrivateProfileStringW(a_section, a_key, L"", buf, 64, a_ini.c_str());
		wchar_t* end = nullptr;
		const float value = std::wcstof(buf, &end);
		return end != buf ? value : a_default;
	}
}

void Settings::Load()
{
	const auto ini = IniPath().wstring();
	enabled = GetPrivateProfileIntW(L"General", L"bEnabled", enabled, ini.c_str()) != 0;
	maxWaitSeconds = std::clamp(ReadFloat(L"General", L"fMaxWaitSeconds", maxWaitSeconds, ini), 1.0f, 60.0f);
	includeFollowers = GetPrivateProfileIntW(L"General", L"bIncludeFollowers", includeFollowers, ini.c_str()) != 0;
	message = ReadString(L"Text", L"sMessage", message, ini);
	messageNoName = ReadString(L"Text", L"sMessageNoName", messageNoName, ini);

	logs::info("Settings: enabled={} maxWait={}s followers={} message='{}'",
		enabled, maxWaitSeconds, includeFollowers, message);
}

void Settings::Save() const
{
	const auto ini = IniPath().wstring();
	const auto wait = std::format(L"{:g}", maxWaitSeconds);
	const bool ok = WritePrivateProfileStringW(L"General", L"bEnabled", enabled ? L"1" : L"0", ini.c_str()) &&
	                WritePrivateProfileStringW(L"General", L"fMaxWaitSeconds", wait.c_str(), ini.c_str()) &&
	                WritePrivateProfileStringW(L"General", L"bIncludeFollowers", includeFollowers ? L"1" : L"0", ini.c_str());

	logs::info("Settings saved{}: enabled={} maxWait={}s followers={}",
		ok ? "" : " (ini write FAILED)", enabled, maxWaitSeconds, includeFollowers);
}
