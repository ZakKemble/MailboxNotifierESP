<?php
/*
 * Project: Mail Notifier - WiFi Edition
 * Author: Zak Kemble, contact@zakkemble.net
 * Copyright: (C) 2022 by Zak Kemble
 * License: 
 * Web: https://blog.zakkemble.net/mail-notifier-wifi-edition/
 */

	// https://apps.timwhitlock.info/emoji/tables/unicode

	// https://stackoverflow.com/questions/9241800/merging-two-complex-objects-in-php
	function my_merge($arr1, $arr2)
	{
		$keys = array_keys($arr2);
		foreach($keys as $key)
		{
			if(
				isset($arr1[$key]) &&
				is_array($arr1[$key]) &&
				is_array($arr2[$key])
			)
				$arr1[$key] = my_merge($arr1[$key], $arr2[$key]);
			else
				$arr1[$key] = $arr2[$key];
		}
		return $arr1;
	}

	// Debugging
	// Read test.json instead of the JSON POST data from the client
	$jsonSourceDebug = false;

	// Log JSON data to log/ directory (make sure it exists and writable)
	$logJsonData = false;

	// Speed things up a bit by spawning curl processes to send the Telegram API requests
	// However, we can't tell if the request was successful with this enabled
	// Most web hosts probably won't allow spawning processes
	$fastResponse = false;

	$TG_TOKEN = '000000000:xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx'; // Telegram bot token
	$TG_CHATID = '-0000000000000'; // Group chat ID

	date_default_timezone_set('Etc/UTC');

	// Blank out headers to reduce response size
	header('Date: ');
	header('Content-Type: ');
	header('Server: ');
	
	$JSON_ERROR = '{"result":"error"}';

	$jsonIn = file_get_contents($jsonSourceDebug ? 'test.json' : 'php://input');
	$jsonLength = strlen($jsonIn);
	if(!$jsonLength || $jsonLength > 4096)
		die($JSON_ERROR);

	if($logJsonData)
	{
		$fileName = gmdate('ymd-His', $_SERVER['REQUEST_TIME']); // TODO what if 2 requests in the same second?
		file_put_contents('log/' . $fileName . '.json', $jsonIn); // TODO use JSON_PRETTY_PRINT
	}
	
	$jsonDefaultIn = file_get_contents('default.json');
	
	$obj = null;
	if(strlen($jsonDefaultIn))
	{
		$json1 = json_decode($jsonDefaultIn, true);
		if($json1 === NULL || json_last_error() != JSON_ERROR_NONE)
		{
			die($JSON_ERROR);
		}
		
		$json2 = json_decode($jsonIn, true);
		if($json2 === NULL || json_last_error() != JSON_ERROR_NONE)
		{
			die($JSON_ERROR);
		}

		$res = my_merge($json1, $json2);
		$obj = json_decode(json_encode($res));
	}

	if($obj === null)
		die($JSON_ERROR);

	$msgData = [];
	if($obj->notify == 'mail')
	{
		$msgData[] = [
			"%s *You have mail!*\n",
			"\xF0\x9F\x93\xA8"
		];
	}
	else if($obj->notify == 'stuck')
	{
		$msgData[] = [
			"%s *Switch stuck!*\n",
			"\xE2\x9A\xA0"
		];
	}
	else
	{
		$msgData[] = [
			"Unknown notify: %s\n",
			$obj->notify
		];
	}
	$msgData[] = [
		"\n",
	];
	$msgData[] = [ // Battery level
		"%s %umV %s\n",
		"\xE2\x9A\xA1",
		$obj->battery,
		($obj->battery > 3600) ? "\xE2\x9C\x85" : "\xE2\x9D\x8C"
	];
	$msgData[] = [ // SHT31 sensor info
		"%s %.2f\xC2\xB0C / %.2f%%\n",
		"\xE2\x9B\x85",
		$obj->env->t,
		$obj->env->h
	];
	$msgData[] = [
		"Counts: *%u/%u/%u* (%u)\n",
		$obj->counts->success,
		$obj->counts->wififail,
		$obj->counts->netfail,
		$obj->lastfail
	];

	// Create final string
	$fmt = '';
	$args = [];
	for($i=0;$i<count($msgData);++$i)
	{
		$fmt .= $msgData[$i][0];
		for($x=1;$x<count($msgData[$i]);++$x)
			$args[] = $msgData[$i][$x];
	}
	$tgMsg = vsprintf($fmt, $args);

	// The main telegram message
	$tgUrl = 'https://api.telegram.org/bot' . $TG_TOKEN . '/sendMessage';
	$tgJsonData = array(
		'chat_id' => $TG_CHATID,
		'parse_mode' => 'markdown',
		'disable_web_page_preview' => true,
		'text' => $tgMsg
	);
	$tgJsonDataEncoded = json_encode($tgJsonData);

	if($fastResponse)
		exec("curl -m 5 -H 'Content-Type: application/json' -d " . escapeshellarg($tgJsonDataEncoded) . " '{$tgUrl}' >/dev/null &");
	else
	{
		$ch = curl_init($tgUrl);
		curl_setopt($ch, CURLOPT_POST, 1);
		curl_setopt($ch, CURLOPT_RETURNTRANSFER, 1);
		curl_setopt($ch, CURLOPT_POSTFIELDS, $tgJsonDataEncoded);
		curl_setopt($ch, CURLOPT_HTTPHEADER, array('Content-Type: application/json')); 
		$tgResponse = curl_exec($ch);
		//echo $tgResponse;
	}

	echo '{"result":"ok"}';
