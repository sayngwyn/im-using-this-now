Scriptname ImUsingThisNow_MCM extends SKI_ConfigBase
{SkyUI menu for I'm Using This Now. The settings live in the SKSE plugin's ini, so they hold for every save.}

int oidEnabled
int oidFollowers
int oidWait

event OnConfigInit()
	ModName = "I'm Using This Now"
endEvent

event OnPageReset(string a_page)
	SetCursorFillMode(TOP_TO_BOTTOM)

	AddHeaderOption("Crafting stations")
	oidEnabled = AddToggleOption("Take over busy stations", ImUsingThisNow.GetEnabled())
	oidFollowers = AddToggleOption("Followers step away too", ImUsingThisNow.GetIncludeFollowers())
	oidWait = AddSliderOption("Wait for the NPC to get up", ImUsingThisNow.GetMaxWaitSeconds(), "{0} s")

	AddEmptyOption()
	AddHeaderOption("Message")
	AddTextOption(ImUsingThisNow.GetMessage(), "", OPTION_FLAG_DISABLED)
	AddTextOption("Change it in ImUsingThisNow.ini", "", OPTION_FLAG_DISABLED)
endEvent

event OnOptionSelect(int a_option)
	if a_option == oidEnabled
		ImUsingThisNow.SetEnabled(!ImUsingThisNow.GetEnabled())
		SetToggleOptionValue(a_option, ImUsingThisNow.GetEnabled())
	elseif a_option == oidFollowers
		ImUsingThisNow.SetIncludeFollowers(!ImUsingThisNow.GetIncludeFollowers())
		SetToggleOptionValue(a_option, ImUsingThisNow.GetIncludeFollowers())
	endif
endEvent

event OnOptionDefault(int a_option)
	if a_option == oidEnabled
		ImUsingThisNow.SetEnabled(true)
		SetToggleOptionValue(a_option, true)
	elseif a_option == oidFollowers
		ImUsingThisNow.SetIncludeFollowers(true)
		SetToggleOptionValue(a_option, true)
	elseif a_option == oidWait
		ImUsingThisNow.SetMaxWaitSeconds(10.0)
		SetSliderOptionValue(a_option, 10.0, "{0} s")
	endif
endEvent

event OnOptionSliderOpen(int a_option)
	if a_option == oidWait
		SetSliderDialogStartValue(ImUsingThisNow.GetMaxWaitSeconds())
		SetSliderDialogDefaultValue(10.0)
		SetSliderDialogRange(1.0, 60.0)
		SetSliderDialogInterval(1.0)
	endif
endEvent

event OnOptionSliderAccept(int a_option, float a_value)
	if a_option == oidWait
		ImUsingThisNow.SetMaxWaitSeconds(a_value)
		SetSliderOptionValue(a_option, ImUsingThisNow.GetMaxWaitSeconds(), "{0} s")
	endif
endEvent

event OnOptionHighlight(int a_option)
	if a_option == oidEnabled
		SetInfoText("Activate a crafting station an NPC is using: they step away and it's yours on the first click.")
	elseif a_option == oidFollowers
		SetInfoText("On: your followers give up a station for you, like any other NPC.\nOff: followers keep using it until they're done, as in vanilla.")
	elseif a_option == oidWait
		SetInfoText("If the NPC hasn't got up by then, the mod gives up and the station stays theirs.")
	endif
endEvent
