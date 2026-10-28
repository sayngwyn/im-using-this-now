Scriptname ImUsingThisNow Hidden
{Settings of I'm Using This Now, provided by its SKSE plugin. Setters apply at once and save to ImUsingThisNow.ini.}

bool function GetEnabled() global native
function SetEnabled(bool a_value) global native

bool function GetIncludeFollowers() global native
function SetIncludeFollowers(bool a_value) global native

float function GetMaxWaitSeconds() global native
function SetMaxWaitSeconds(float a_value) global native

string function GetMessage() global native
