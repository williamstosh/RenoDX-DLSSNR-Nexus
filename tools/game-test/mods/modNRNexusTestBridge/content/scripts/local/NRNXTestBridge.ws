exec function NRNX_TestBridgeReady(requestId : string)
{
	LogChannel( 'NRNXTestBridge', "NRNX|" + requestId + "|ready|1" );
}

exec function NRNX_TestBridgeLoadSave(filename : string, requestId : string)
{
	var saves : array< SSavegameInfo >;
	var savesInitialized : bool;
	var listSucceeded : bool;
	var matchIndex : int;
	var matchCount : int;
	var sampleName : string;
	var i : int;

	savesInitialized = theGame.AreSavesInitialized();
	if ( !savesInitialized )
	{
		LogChannel( 'NRNXTestBridge', "NRNX|" + requestId + "|load|pending|saves_not_initialized" );
		return;
	}

	listSucceeded = theGame.ListSavedGames( saves );
	if ( !listSucceeded )
	{
		LogChannel( 'NRNXTestBridge', "NRNX|" + requestId + "|load|list_failed|save_count|" + saves.Size() );
		return;
	}

	matchIndex = -1;
	matchCount = 0;
	for ( i = 0; i < saves.Size(); i += 1 )
	{
		if ( saves[i].filename == filename )
		{
			matchIndex = i;
			matchCount += 1;
		}
	}

	if ( matchCount != 1 )
	{
		sampleName = "";
		if ( saves.Size() > 0 )
		{
			sampleName = saves[0].filename;
		}
		LogChannel( 'NRNXTestBridge', "NRNX|" + requestId + "|load|match_count|" + matchCount + "|save_count|" + saves.Size() + "|sample|" + sampleName );
		return;
	}

	LogChannel( 'NRNXTestBridge', "NRNX|" + requestId + "|load|requested|" + filename );
	theGame.LoadGameInit( saves[matchIndex], false );
}

exec function NRNX_TestBridgeState(requestId : string)
{
	var loadResult : ELoadGameResult;
	loadResult = theGame.GetLoadGameProgress();

	if ( loadResult == LOAD_MissingContent )
	{
		LogChannel( 'NRNXTestBridge', "NRNX|" + requestId + "|state|missing_content" );
	}
	else if ( loadResult == LOAD_Error )
	{
		LogChannel( 'NRNXTestBridge', "NRNX|" + requestId + "|state|error" );
	}
	else if ( thePlayer )
	{
		LogChannel( 'NRNXTestBridge', "NRNX|" + requestId + "|state|gameplay_ready" );
	}
	else if ( loadResult == LOAD_NotInitialized || loadResult == LOAD_Initializing || loadResult == LOAD_Loading )
	{
		LogChannel( 'NRNXTestBridge', "NRNX|" + requestId + "|state|loading" );
	}
	else
	{
		LogChannel( 'NRNXTestBridge', "NRNX|" + requestId + "|state|waiting" );
	}
}

exec function NRNX_TestBridgeExit(requestId : string)
{
	LogChannel( 'NRNXTestBridge', "NRNX|" + requestId + "|exit|requested" );
	theGame.RequestExit();
}
