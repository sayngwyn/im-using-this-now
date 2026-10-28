#include "pch.h"

#include "Papyrus.h"
#include "Settings.h"

namespace
{
	constexpr auto kScript = "ImUsingThisNow";

	// Each setter applies at once and saves to the ini, so the menu and the
	// ini always agree and the choice holds for every save.
	bool GetEnabled(RE::StaticFunctionTag*) { return Settings::Get().enabled; }
	void SetEnabled(RE::StaticFunctionTag*, bool a_value)
	{
		Settings::Get().enabled = a_value;
		Settings::Get().Save();
	}

	bool GetIncludeFollowers(RE::StaticFunctionTag*) { return Settings::Get().includeFollowers; }
	void SetIncludeFollowers(RE::StaticFunctionTag*, bool a_value)
	{
		Settings::Get().includeFollowers = a_value;
		Settings::Get().Save();
	}

	float GetMaxWaitSeconds(RE::StaticFunctionTag*) { return Settings::Get().maxWaitSeconds; }
	void SetMaxWaitSeconds(RE::StaticFunctionTag*, float a_value)
	{
		Settings::Get().maxWaitSeconds = std::clamp(a_value, 1.0f, 60.0f);
		Settings::Get().Save();
	}

	// The message as the menu shows it. SkyUI drops braces from menu text, so
	// "{name}" would read as "name".
	RE::BSFixedString GetMessage(RE::StaticFunctionTag*)
	{
		std::string text = Settings::Get().message;
		for (auto pos = text.find("{name}"); pos != std::string::npos; pos = text.find("{name}", pos)) {
			text.replace(pos, 6, "[NPC name]");
		}
		return text;
	}

	bool Register(RE::BSScript::IVirtualMachine* a_vm)
	{
		a_vm->RegisterFunction("GetEnabled", kScript, GetEnabled);
		a_vm->RegisterFunction("SetEnabled", kScript, SetEnabled);
		a_vm->RegisterFunction("GetIncludeFollowers", kScript, GetIncludeFollowers);
		a_vm->RegisterFunction("SetIncludeFollowers", kScript, SetIncludeFollowers);
		a_vm->RegisterFunction("GetMaxWaitSeconds", kScript, GetMaxWaitSeconds);
		a_vm->RegisterFunction("SetMaxWaitSeconds", kScript, SetMaxWaitSeconds);
		a_vm->RegisterFunction("GetMessage", kScript, GetMessage);
		return true;
	}
}

void Papyrus::Install()
{
	SKSE::GetPapyrusInterface()->Register(Register);
}
