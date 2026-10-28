#pragma once

// Settings from ImUsingThisNow.ini, next to the DLL (Data\SKSE\Plugins).
struct Settings
{
	bool        enabled = true;
	float       maxWaitSeconds = 10.0f;  // give up if the NPC hasn't left by then
	bool        includeFollowers = true;
	std::string message = "I'm using this now. Get lost, {name}.";
	std::string messageNoName = "I'm using this now.";

	static Settings& Get()
	{
		static Settings instance;
		return instance;
	}

	void Load();
	// Writes the menu's settings (not the texts) back to the ini.
	void Save() const;
};
