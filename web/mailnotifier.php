<?php
/*
 * Project: Mail Notifier - WiFi Edition
 * Author: Zak Kemble, contact@zakkemble.net
 * Copyright: (C) 2022 by Zak Kemble
 * License: 
 * Web: https://blog.zakkemble.net/mail-notifier-wifi-edition/
 */

	// https://apps.timwhitlock.info/emoji/tables/unicode

	// Debugging
	// Read test.json instead of the JSON POST data from the client
	define('JSON_SOURCE_DEBUG', false);

	// Log JSON data to log/ directory (make sure it exists and writable)
	define('LOG_JSON_DATA', false);

	// Speed things up a bit by spawning curl processes to send the Telegram API requests.
	// However, we can't tell if the request was successful with this enabled.
	// Most web hosts probably won't allow spawning processes.
	define('FAST_RESPONSE', false);

	// Telegram bot params
	define('TG_TOKEN', '000000000:xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx'); // Bot token
	define('TG_CHATID', '-0000000000000'); // Group chat ID

	// Responses to client
	define('JSON_RESP_ERROR', '{"result":"error"}');
	define('JSON_RESP_OK', '{"result":"ok"}');


	date_default_timezone_set('Etc/UTC');
	header('Content-Type: application/json');

	if(!JSON_SOURCE_DEBUG)
	{
		if(
			$_SERVER['REQUEST_METHOD'] !== 'POST'
//			|| !isset($_SERVER['HTTP_X_KEY'])
//			|| $_SERVER['HTTP_X_KEY'] !== 'aaaaaabbbbbccccc22222444444aaaffffff7777'
		)
			die(JSON_RESP_ERROR);
	}

	// Get the data supplied in the POST request
	$jsonIn = file_get_contents(JSON_SOURCE_DEBUG ? 'test.json' : 'php://input');
	$jsonLength = strlen($jsonIn);
	if(!$jsonLength || $jsonLength > 4096)
		die(JSON_RESP_ERROR);

	if(LOG_JSON_DATA)
	{
		$fileName = gmdate('ymd-His', $_SERVER['REQUEST_TIME']); // TODO what if 2 requests in the same second?
		file_put_contents('log/' . $fileName . '.json', $jsonIn); // TODO use JSON_PRETTY_PRINT
	}

	$obj = json_decode($jsonIn, true);
	if($obj === NULL || json_last_error() != JSON_ERROR_NONE)
		die(JSON_RESP_ERROR);


	// Make sure all of the values we're going to look at are defined in someway
	$obj['notify']				= $obj['notify'] ?? "";
	$obj['battery']				= $obj['battery'] ?? 0;
	$obj['env']['t']			= $obj['env']['t'] ?? 0;
	$obj['env']['h']			= $obj['env']['h'] ?? 0;
	$obj['counts']['success']	= $obj['counts']['success'] ?? 0;
	$obj['counts']['wififail']	= $obj['counts']['wififail'] ?? 0;
	$obj['counts']['netfail']	= $obj['counts']['netfail'] ?? 0;
	$obj['lastfail']			= $obj['lastfail'] ?? 0;


	// Create message
	$tgMsg = '';
	if($obj['notify'] == 'mail')
		$tgMsg = "\xF0\x9F\x93\xA8 *You have mail!*\n";
	else if($obj['notify'] == 'stuck')
		$tgMsg = "\xE2\x9A\xA0 *Switch stuck!*\n";
	else
		$tgMsg = sprintf("Unknown notify: %s\n", $obj['notify']);

	$tgMsg = sprintf(
		$tgMsg .
		"\n" .
		"\xE2\x9A\xA1 %umV %s\n" . // Battery level
		"\xE2\x9B\x85 %.2f\xC2\xB0C / %.2f%%\n" . // SHT31 sensor info
		"Counts: *%u/%u/%u* (%u)\n",
		$obj['battery'],
		($obj['battery'] > 3600) ? "\xE2\x9C\x85" : "\xE2\x9D\x8C",
		$obj['env']['t'],
		$obj['env']['h'],
		$obj['counts']['success'],
		$obj['counts']['wififail'],
		$obj['counts']['netfail'],
		$obj['lastfail']
	);


	// Create telegram request
	$tgUrl = 'https://api.telegram.org/bot' . TG_TOKEN . '/sendMessage';
	$tgJsonData = [
		'chat_id'					=> TG_CHATID,
		'parse_mode'				=> 'markdown',
		'disable_web_page_preview'	=> true,
		'text'						=> $tgMsg
	];
	$tgJsonDataEncoded = json_encode($tgJsonData);

	// Send the message
	if(FAST_RESPONSE)
		exec("curl -m 5 -H 'Content-Type: application/json' -d " . escapeshellarg($tgJsonDataEncoded) . " '{$tgUrl}' >/dev/null &");
	else
	{
		$ch = curl_init();
		curl_setopt_array($ch, [
			CURLOPT_CONNECTTIMEOUT	=> 2000,
			CURLOPT_TIMEOUT			=> 4000,
			CURLOPT_POST			=> 1,
			CURLOPT_RETURNTRANSFER	=> 1,
			CURLOPT_POSTFIELDS		=> $tgJsonDataEncoded,
			CURLOPT_HTTPHEADER		=> ['Content-Type: application/json'],
			CURLOPT_URL				=> $tgUrl
		]);
		$tgResponse = curl_exec($ch);
		//echo $tgResponse;
	}

	die(JSON_RESP_OK);
