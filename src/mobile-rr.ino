//
// ESP8266 Mobile Rick Roll Captive Portal
// - User connects thinking it is a free WiFi access point
// - After accepting Terms of Service they are Rick Roll'd
//
// References (I learned everything I needed to create this from these projects)
// PlatformIO - http://platformio.org/
//              http://docs.platformio.org/en/latest/platforms/espressif.html#uploading-files-to-file-system-spiffs
// ESP8266 Captive Portal - https://www.hackster.io/rayburne/esp8266-captive-portal-5798ff
// ESP-RADIO - https://github.com/Edzelf/Esp-radio
// ESPAsyncWebServer - https://github.com/me-no-dev/ESPAsyncWebServer
// SOFTAP - Get List of Connected Clients - http://www.esp8266.com/viewtopic.php?f=32&t=5669
// How to print free memory - http://forum.wemos.cc/topic/175/how-to-print-free-memory
// WebSocketToSerial - https://github.com/hallard/WebSocketToSerial.git
// JQueryTerminal - https://github.com/jcubic/jquery.terminal
// OTA Update - http://www.penninkhof.com/2015/12/1610-over-the-air-esp8266-programming-using-platformio/
// Piezo Beep - http://framistats.com/2015/06/07/esp8266-arduino-smtp-server-part-2/
// Simple Audio Board - http://bitcows.com/?p=19
// Tone Doesn't Work - https://www.reddit.com/r/esp8266/comments/46kw38/esp8266_calling_tone_function_does_not_work/
//

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <stdio.h>
#include <string.h>

#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <FS.h>
#include <Hash.h>

#include "DNSServer.h"

extern "C"
{
#include "user_interface.h"

	void system_set_os_print ( uint8 onoff );
	void ets_install_putc1 ( void *routine );
}

ADC_MODE ( ADC_VCC ); 															// Set ADC for Voltage Monitoring

// Use the internal hardware buffer
static void _u0_putc ( char c )
{
	while ( ( ( U0S >> USTXC ) & 0x7F ) == 0x7F );

	U0F = c;
}

//***************************************************************************
// Global data section.														*
//***************************************************************************
char  				ssid[] = "FREE Highspeed WiFi";
bool		 		DEBUG = 1;

#define PIEZO_PIN		4

// Maximum number of simultaneous clients connected (WebSocket)
#define MAX_WS_CLIENT   5

#define CLIENT_NONE     0
#define CLIENT_ACTIVE   1

#define HELP_TEXT "[[b;green;]ESP8266 Mobile Rick Roll]\n" \
                  "------------------------\n" \
                  "[[b;cyan;]?] or [[b;cyan;]help] show this help\n" \
				  "[[b;cyan;]beep n]    sound piezo for 'n' ms\n" \
                  "[[b;cyan;]count]     show Rick Roll count\n" \
				  "[[b;cyan;]debug]     toggle debug output\n" \
                  "[[b;cyan;]info]      show system information\n" \
                  "[[b;cyan;]ls]        list SPIFFS files\n" \
                  "[[b;cyan;]ping]      send ping command\n" \
                  "[[b;cyan;]reboot]    reboot system\n" \
				  "[[b;cyan;]ssid 's']  change ssid to 's'\n" \
                  "[[b;cyan;]who]       show all clients connected\n" \
                  "[[b;cyan;]whoami]    show your client #"


// Web Socket client state
typedef struct
{
	uint32_t  id;
	uint8_t   state;
} _ws_client;

IPAddress			ip ( 10, 10, 10, 1 );                  						// Private network for httpd
DNSServer			dnsd;                            							// Create the DNS object

AsyncWebServer		httpd ( 80 );                      							// Instance of embedded webserver
AsyncWebSocket		ws ( "/ws" );                        						// access at ws://[esp ip]/ws
_ws_client			ws_client[MAX_WS_CLIENT];               					// State Machine for WebSocket Client;

int					rrcount;													// Rick Roll Counter
char 				str_vcc[8];

//***************************************************************************
// End of global data section.												*
//***************************************************************************

//***************************************************************************
//                            D B G P R I N T								*
//***************************************************************************
void dbg_printf ( const char *format, ... )
{
	static char sbuf[1400] ;               										// For debug lines
	va_list varArgs ;                                    						// For variable number of params

	va_start ( varArgs, format ) ;                      						// Prepare parameters
	vsnprintf ( sbuf, sizeof ( sbuf ), format, varArgs ) ;  					// Format the message
	va_end ( varArgs ) ;                                 						// End of using parameters

	Serial.println ( sbuf ) ;

	if ( DEBUG )			                                        			// DEBUG on?
	{
		if ( ws.count() )
			ws.textAll ( sbuf );
	}
}

//***************************************************************************
//                    F O R M A T  B Y T E S			       	    		*
//***************************************************************************
String formatBytes ( size_t bytes )
{
	if ( bytes < 1024 )
	{
		return String ( bytes ) + " B";
	}
	else if ( bytes < ( 1024 * 1024 ) )
	{
		return String ( bytes / 1024.0 ) + " KB";
	}
	else if ( bytes < ( 1024 * 1024 * 1024 ) )
	{
		return String ( bytes / 1024.0 / 1024.0 ) + " MB";
	}
	else
	{
		return String ( bytes / 1024.0 / 1024.0 / 1024.0 ) + " GB";
	}
}

//***************************************************************************
//                    P I E Z O   B E E P									*
//***************************************************************************
void beep ( int delayms )
{
	delayms = delayms * 100000;

	while ( delayms > 0 )
	{
		digitalWrite ( PIEZO_PIN, HIGH );										// Turn PIEZO on
		delay ( 1000 );															// wait for a delayms ms

		delayms -= 1000;
	}

	digitalWrite ( PIEZO_PIN, LOW );       										// Turn PIEZO off
}

void beepC ( int delayms )
{
	for ( int c = 0; c < 448; c++ )
	{
		digitalWrite ( PIEZO_PIN, HIGH );
		delayMicroseconds ( delayms );
		digitalWrite ( PIEZO_PIN, LOW );
		delayMicroseconds ( delayms );
	}

	delay ( 100 );

	for ( int c = 0; c < 224; c++ )
	{
		digitalWrite ( PIEZO_PIN, HIGH );
		delayMicroseconds ( delayms - ( delayms * .1 ) );
		digitalWrite ( PIEZO_PIN, LOW );
		delayMicroseconds ( delayms - ( delayms * .1 ) );
	}
}

void beep_rr( bool multiplier = false )
{
	// We'll set up an array with the notes we want to play
	// change these values to make different songs!

	// Length must equal the total number of notes and spaces

	const int songLength = 18;

	// Notes is an array of text characters corresponding to the notes
	// in your song. A space represents a rest (no tone)

	char notes[] = "cdfda ag cdfdg gf "; // a space represents a rest

	// Beats is an array values for each note and rest.
	// A "1" represents a quarter-note, 2 a half-note, etc.
	// Don't forget that the rests (spaces) need a length as well.

	int beats[] = {1, 1, 1, 1, 1, 1, 4, 4, 2, 1, 1, 1, 1, 1, 1, 4, 4, 2};

	// The tempo is how fast to play the song.
	// To make the song play faster, decrease this value.

	int tempo = 125;
	if (multiplier)
		tempo = tempo * 100000;

	dbg_printf("Tempo %d\n", tempo);

	int i, duration;

	digitalWrite ( LED_BUILTIN, LOW );

	for ( i = 0; i < songLength; i++ ) // step through the song arrays
	{
		duration = beats[i] * tempo;  // length of note/rest in ms

		if ( notes[i] == ' ' )        // is this a rest?
		{
			delay ( duration );         // then pause for a moment
		}
		else                          // otherwise, play the note
		{
			analogWriteFreq ( frequency ( notes[i] ) );
			analogWrite ( PIEZO_PIN, 800 );
			delay ( duration );
			analogWrite ( PIEZO_PIN, 0 );
		}

		delay ( tempo / 10 );         // brief pause between notes
	}

	digitalWrite ( LED_BUILTIN, HIGH );
}

int frequency ( char note )
{
	// This function takes a note character (a-g), and returns the
	// corresponding frequency in Hz for the tone() function.

	int i;
	const int numNotes = 8;  // number of notes we're storing

	// The following arrays hold the note characters and their
	// corresponding frequencies. The last "C" note is uppercase
	// to separate it from the first lowercase "c". If you want to
	// add more notes, you'll need to use unique characters.

	// For the "char" (character) type, we put single characters
	// in single quotes.

	char names[] = { 'c', 'd', 'e', 'f', 'g', 'a', 'b', 'C' };
	int frequencies[] = {262, 294, 330, 349, 392, 440, 494, 523};

	// Now we'll search through the letters in the array, and if
	// we find it, we'll return the frequency for that note.

	for ( i = 0; i < numNotes; i++ ) // Step through the notes
	{
		if ( names[i] == note )       // Is this the one?
		{
			return ( frequencies[i] );  // Yes! Return the frequency
		}
	}

	return ( 0 ); // We looked through everything and didn't find it,
	// but we still need to return a value, so return 0.
}

//***************************************************************************
//                    S E T U P												*
//***************************************************************************
void setup ( void )
{
	uint8_t		mac[6];

	ets_install_putc1 ( ( void * ) &_u0_putc );
	system_set_os_print ( 1 );
//	system_update_cpu_freq ( 160 ) ;                      						// Set CPU to 80/160 MHz

	Serial.begin ( 115200 ) ;                         							// For debug
	Serial.println() ;


	pinMode ( LED_BUILTIN, OUTPUT );                    						// initialize onboard LED as output
	digitalWrite ( LED_BUILTIN, HIGH );	                   						// Turn the LED off by making the voltage HIGH


	pinMode ( PIEZO_PIN, OUTPUT );                    							// initialize PIEZO PIN as output
	beep_rr();

	// Setup Access Point
	setupAP();
	WiFi.softAPmacAddress ( mac );

	// Startup Banner
	dbg_printf (
		"--------------------\n"
		"ESP8266 Mobile Rick Roll Captive Portal\n"
	);

	// Show Soft AP Info
	dbg_printf (
		"SoftAP MAC: %02X:%02X:%02X:%02X:%02X:%02X\n" \
		"SoftAP IP: " IPSTR "\n",
		MAC2STR ( mac ),
		IP2STR ( ip )
	) ;

	dbg_printf ( "SYSTEM ---" );
	dbg_printf ( "getSdkVersion:      %s", ESP.getSdkVersion() );
	dbg_printf ( "getBootVersion:     %08X", ESP.getBootVersion() );
	dbg_printf ( "getBootMode:        %08X", ESP.getBootMode() );
	dbg_printf ( "getChipId:          %08X", ESP.getChipId() );
	dbg_printf ( "getCpuFreqMHz:      %d Mhz", ESP.getCpuFreqMHz() );
	dbg_printf ( "getCycleCount:      %u\n", ESP.getCycleCount() );

	dbg_printf ( "POWER ---" );
	dbg_printf ( "Voltage:            %d.%d v\n ", (ESP.getVcc() / 1000), (ESP.getVcc() % 1000) );

	dbg_printf ( "MEMORY ---" );
	dbg_printf ( "getFreeHeap:        %d B", ESP.getFreeHeap() );
	dbg_printf ( "getSketchSize:      %s", formatBytes ( ESP.getSketchSize() ).c_str() );
	dbg_printf ( "getFreeSketchSpace: %s", formatBytes ( ESP.getFreeSketchSpace() ).c_str() );
	dbg_printf ( "getFlashChipSize:   %s", formatBytes ( ESP.getFlashChipRealSize() ).c_str() );
	dbg_printf ( "getFlashChipSpeed:  %d MHz\n", int ( ESP.getFlashChipSpeed() / 1000000 ) );

	setupSPIFFS();

	// Setup DNS Server
	// if DNS Server is started with "*" for domain name,
	// it will reply with provided IP to all DNS request
	dbg_printf ( "Starting DNS Server" ) ;
	dnsd.onQuery ( []( const IPAddress &remoteIP, const char* domain, const IPAddress &resolvedIP )
	{
		dbg_printf ( "DNS[%d]: %s -> " IPSTR, remoteIP[3], domain, IP2STR(resolvedIP));
	} );
	dnsd.start ( 53, "*", ip );

	setupHTTPServer();
	setupOTAServer();

	dbg_printf ( "\nReady!\n--------------------" ) ;
}

void setupAP()
{
	WiFi.mode ( WIFI_AP );
	WiFi.softAPConfig ( ip, ip, IPAddress ( 255, 255, 255, 0 ) );
	WiFi.softAP ( ssid );
}

void setupSPIFFS()
{
	FSInfo		fs_info ;                       								// Info about SPIFFS
	Dir			dir ;                         									// Directory struct for SPIFFS
	File		f ;                           									// Filehandle
	String		filename ;                        								// Name of file found in SPIFFS

	SPIFFS.begin() ;                              								// Enable file system

	// Show some info about the SPIFFS
	uint16_t cnt = 0;
	SPIFFS.info ( fs_info ) ;
	dbg_printf ( "SPIFFS Files\nName                           -      Size" );
	dir = SPIFFS.openDir ( "/" ) ;                      						// Show files in FS

	while ( dir.next() )                            							// All files
	{
		f = dir.openFile ( "r" ) ;
		filename = dir.fileName() ;
		dbg_printf ( "%-30s - %9s",                      						// Show name and size
					 filename.c_str(),
					 formatBytes ( f.size() ).c_str()
				   ) ;
		cnt++;
	}

	dbg_printf ( "%d Files, %s of %s Used",
				 cnt,
				 formatBytes ( fs_info.usedBytes ).c_str(),
				 formatBytes ( fs_info.totalBytes ).c_str()
			   );
	dbg_printf ( "%s Free\n",
				 formatBytes ( fs_info.totalBytes - fs_info.usedBytes ).c_str()
			   );
}

void setupHTTPServer()
{
	// Web Server Document Setup
	dbg_printf ( "Starting HTTP Captive Portal" ) ;

	httpd.on ( "/", HTTP_GET, onRequest ) ;                  					// Handle startpage
	httpd.onNotFound ( onRequest ) ;                     						// Handle file from FS

	httpd.on ( "/console", HTTP_GET, [] ( AsyncWebServerRequest * request )
	{
		request->send ( SPIFFS, "/console.htm" ) ;
	} );

	httpd.on ( "/trigger", HTTP_GET, [] ( AsyncWebServerRequest * request )
	{
		AsyncWebHeader *h = request->getHeader ( "User-Agent" );

		rrcount++;
		IPAddress remoteIP = request->client()->remoteIP();
		ws.printfAll ( "[[b;yellow;]Rick Roll Sent!] (%d): [" IPSTR "] %s",
					   rrcount,
					   IP2STR ( remoteIP ),
					   h->value().c_str()
					 ) ;
		request->send ( 200, "text/html", String ( rrcount ) ) ;
		beepC ( 200 );
	} );

	// attach AsyncWebSocket
	dbg_printf ( "Starting Websocket Console" ) ;
	ws.onEvent ( onEvent );
	httpd.addHandler ( &ws );

	httpd.begin() ;
}

void setupOTAServer()
{
	dbg_printf ( "Starting OTA Update Server" ) ;

	// Port defaults to 8266
	// ArduinoOTA.setPort(8266);

	// Hostname defaults to esp8266-[ChipID]
	ArduinoOTA.setHostname("mobile-rr");

	// No authentication by default
	// ArduinoOTA.setPassword((const char *)"123");

	// OTA callbacks
	ArduinoOTA.onStart ( []()
	{
		ws.enable ( false );													// Disable client connections
		dbg_printf ( "OTA Update Started" );									// Let connected clients know what's going on
	} );
	ArduinoOTA.onEnd ( []()
	{
		dbg_printf ( "OTA Update Complete!\n" );

		if ( ws.count() )
		{
			ws.closeAll();  														// Close connected clients
			delay ( 1000 );
		}

	} );
	ArduinoOTA.onProgress ( [] ( unsigned int progress, unsigned int total )
	{
		dbg_printf ( "Progress: %u%%\r", ( progress / ( total / 100 ) ) );
	} );
	ArduinoOTA.onError ( [] ( ota_error_t error )
	{
		dbg_printf ( "Error[%u]: ", error );

		if ( error == OTA_AUTH_ERROR ) dbg_printf ( "Auth Failed" );
		else if ( error == OTA_BEGIN_ERROR ) dbg_printf ( "Begin Failed" );
		else if ( error == OTA_CONNECT_ERROR ) dbg_printf ( "Connect Failed" );
		else if ( error == OTA_RECEIVE_ERROR ) dbg_printf ( "Receive Failed" );
		else if ( error == OTA_END_ERROR ) dbg_printf ( "End Failed" );
	} );
	ArduinoOTA.begin();
}

//***************************************************************************
//                    L O O P												*
//***************************************************************************
// Main program loop.                                                       *
//***************************************************************************
void loop ( void )
{
	dnsd.processNextRequest();
	ArduinoOTA.handle();  // Handle remote Wifi Updates
}

//***************************************************************************
// HTTPD onRequest															*
//***************************************************************************
void onRequest ( AsyncWebServerRequest *request )
{
	digitalWrite ( LED_BUILTIN, LOW );                    						// Turn the LED on by making the voltage LOW

	IPAddress remoteIP = request->client()->remoteIP();
	dbg_printf (
		"HTTP[%d]: %s%s",
		remoteIP[3],
		request->host().c_str(),
		request->url().c_str()
	) ;

	String path = request->url();

	if ( path.endsWith ( "/" ) )
		path += "index.htm";

	if ( !SPIFFS.exists ( path ) && !SPIFFS.exists ( path + ".gz" ) )
	{
		//AsyncWebHeader *h = request->getHeader ( "User-Agent" );

		// Redirect to captive portal
		//dbg_printf ( "HTTP[%d]: Redirected to captive portal\n%s", remoteIP[3], h->value().c_str() ) ;
		request->redirect ( "http://freewifi.com/index.htm" );
	}
	else
	{
		if ( !request->hasParam ( "download" ) && SPIFFS.exists ( path + ".gz" ) )
		{
			request->send ( SPIFFS, path, String(), request->hasParam ( "download" ) );
		}
		else
		{
			request->send ( SPIFFS, path ) ;                						// Okay, send the file
		}
	}

	digitalWrite ( LED_BUILTIN, HIGH );                   						// Turn the LED off by making the voltage HIGH
}


//***************************************************************************
// WebSocket onEvent														*
//***************************************************************************
// Manage routing of websocket events                                       *
//***************************************************************************
void onEvent ( AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len )
{
	if ( type == WS_EVT_CONNECT )
	{
		uint8_t index;
		//dbg_printf ( "ws[%s][%u] connect\n", server->url(), client->id() );

		for ( index = 0; index < MAX_WS_CLIENT ; index++ )
		{
			if ( ws_client[index].id == 0 )
			{
				ws_client[index].id = client->id();
				ws_client[index].state = CLIENT_ACTIVE;
				//dbg_printf ( "added #%u at index[%d]\n", client->id(), index );
				client->printf ( "[[b;green;]Hello Client #%u, added you at index %d]", client->id(), index );
				client->ping();
				break; // Exit for loop
			}
		}

		if ( index >= MAX_WS_CLIENT )
		{
			//dbg_printf ( "not added, table is full" );
			client->printf ( "[[b;red;]Sorry client #%u, Max client limit %d reached]", client->id(), MAX_WS_CLIENT );
			client->ping();
		}
	}
	else if ( type == WS_EVT_DISCONNECT )
	{
		//dbg_printf ( "ws[%s][%u] disconnect: %u\n", server->url(), client->id() );

		for ( uint8_t i = 0; i < MAX_WS_CLIENT ; i++ )
		{
			if ( ws_client[i].id == client->id() )
			{
				ws_client[i].id = 0;
				ws_client[i].state = CLIENT_NONE;
				//dbg_printf ( "freed[%d]\n", i );
				break; // Exit for loop
			}
		}
	}
	else if ( type == WS_EVT_ERROR )
	{
		dbg_printf ( "WS[%u]: error(%u) - %s", client->id(), * ( ( uint16_t * ) arg ), ( char * ) data );
	}
	else if ( type == WS_EVT_PONG )
	{
		dbg_printf ( "WS[%u]: pong", client->id(), len );
	}
	else if ( type == WS_EVT_DATA )
	{
		//data packet
		AwsFrameInfo *info = ( AwsFrameInfo * ) arg;
		char *msg = NULL;
		size_t n = info->len;
		uint8_t index;
		// Size of buffer needed
		// String same size +1 for \0
		// Hex size*3+1 for \0 (hex displayed as "FF AA BB ...")
		n = info->opcode == WS_TEXT ? n + 1 : n * 3 + 1;
		msg = ( char * ) calloc ( n, sizeof ( char ) );

		if ( msg )
		{
			// Grab all data
			for ( size_t i = 0; i < info->len; i++ )
			{
				if ( info->opcode == WS_TEXT )
				{
					msg[i] = ( char ) data[i];
				}
				else
				{
					sprintf_P ( msg + i * 3, PSTR ( "%02x " ), ( uint8_t ) data[i] );
				}
			}
		}

		//dbg_printf ( "ws[%s][%u] message %s\n", server->url(), client->id(), msg );

		// Search if it's a known client
		for ( index = 0; index < MAX_WS_CLIENT ; index++ )
		{
			if ( ws_client[index].id == client->id() )
			{
				//dbg_printf ( "known[%d] '%s'\n", index, msg );
				//dbg_printf ( "client #%d info state=%d\n", client->id(), ws_client[index].state );

				// Received text message
				if ( info->opcode == WS_TEXT )
				{
					execCommand ( client, msg );
				}
				else
				{
					dbg_printf ( "Binary 0x:%s", msg );
				}

				// Exit for loop
				break;
			} // if known client
		} // for all clients

		// Free up allocated buffer
		if ( msg )
			free ( msg );
	} // EVT_DATA
}

//***************************************************************************
// WebSocket execCommand													*
//***************************************************************************
// translate and execute command                                            *
//***************************************************************************
void execCommand ( AsyncWebSocketClient * client, char * msg )
{
	uint16_t l = strlen ( msg );
	uint8_t index = MAX_WS_CLIENT;

	// Search if we're known client
	if ( client )
	{
		for ( index = 0; index < MAX_WS_CLIENT ; index++ )
		{
			// Exit for loop if we are there
			if ( ws_client[index].id == client->id() )
				break;
		} // for all clients
	}
	else
	{
		return;
	}

	// Custom command to talk to device
	if ( !strcasecmp_P ( msg, PSTR ( "ping" ) ) )
	{
		client->printf_P ( PSTR ( "received your [[b;cyan;]ping], here is my [[b;cyan;]pong]" ) );

	}
	else if ( !strcasecmp_P ( msg, PSTR ( "debug" ) ) )
	{
		DEBUG = !DEBUG;
		if ( DEBUG )
		{
			client->printf_P ( PSTR ( "[[b;green;]Debug output enabled]" ) );
		}
		else
		{
			client->printf_P ( PSTR ( "[[b;red;]Debug output disabled]" ) );
		}

	}
	// Dir files on SPIFFS system
	// --------------------------
	else if ( !strcasecmp_P ( msg, PSTR ( "ls" ) ) )
	{
		FSInfo fs_info ;
		uint16_t cnt = 0;
		String filename;
		client->printf_P ( PSTR ( "[[b;green;]SPIFFS Files]\r\nName                           -      Size" ) );
		Dir dir = SPIFFS.openDir ( "/" ) ;                    					// Show files in FS
		while ( dir.next() )                            						// All files
		{
			cnt++;
			File f = dir.openFile ( "r" ) ;
			filename = dir.fileName() ;
			client->printf_P ( PSTR ( "%-30s - %9s" ) ,                       	// Show name and size
							filename.c_str(),
							formatBytes ( f.size() ).c_str()
						) ;
			f.close();
		}

		SPIFFS.info ( fs_info ) ;
		client->printf_P ( PSTR ( "%d Files, %s of %s Used" ) ,
						   cnt,
						   formatBytes ( fs_info.usedBytes ).c_str(),
						   formatBytes ( fs_info.totalBytes ).c_str()
						 ) ;
		client->printf_P ( PSTR ( "%s Free" ) ,
						   formatBytes ( fs_info.totalBytes - fs_info.usedBytes ).c_str()
						 );
	}
	else if ( !strcasecmp_P ( msg, PSTR ( "whoami" ) ) )
	{
		client->printf_P ( PSTR ( "[[b;green;]You are client #%u at index[%d&#93;]" ), client->id(), index );
	}
	else if ( !strcasecmp_P ( msg, PSTR ( "who" ) ) )
	{
		uint8_t cnt = 0;

		// Count client
		for ( uint8_t i = 0; i < MAX_WS_CLIENT ; i++ )
		{
			if ( ws_client[i].id )
			{
				cnt++;
			}
		}

		client->printf_P ( PSTR ( "[[b;green;]Current client total %d/%d possible]" ), cnt, MAX_WS_CLIENT );

		for ( uint8_t i = 0; i < MAX_WS_CLIENT ; i++ )
		{
			if ( ws_client[i].id )
			{
				client->printf_P ( PSTR ( "index[[[b;green;]%d]]; client [[b;green;]#%d]" ), i, ws_client[i].id );
			}
		}
	}
	else if ( !strcasecmp_P ( msg, PSTR ( "info" ) ) )
	{
		uint8_t mac[6];
		WiFi.softAPmacAddress ( mac );
		client->printf_P ( PSTR ( "[[b;green;]SYSTEM INFORMATION]" ) );
		client->printf_P ( PSTR ( "Firmware version [[b;green;]%s %s]\n " ), __DATE__, __TIME__ );

		client->printf_P ( PSTR ( "[[b;cyan;]SYSTEM]" ) );
		client->printf_P ( PSTR ( "SDK version:  %s" ), ESP.getSdkVersion() );
		client->printf_P ( PSTR ( "Boot Version: %08X" ), ESP.getBootVersion() );
		client->printf_P ( PSTR ( "Boot Mode:    %08X" ), ESP.getBootMode() );
		client->printf_P ( PSTR ( "Chip ID:      %08X" ), ESP.getChipId() );
		client->printf_P ( PSTR ( "CPU Freq:     %d MHz" ), ESP.getCpuFreqMHz() );
		client->printf_P ( PSTR ( "Cycle Count:  %u\n " ), ESP.getCycleCount() );

		client->printf_P ( PSTR ( "[[b;cyan;]POWER]" ) );
		client->printf_P ( PSTR ( "Voltage:      %d.%d v\n " ), (ESP.getVcc() / 1000), (ESP.getVcc() % 1000) );

		client->printf_P ( PSTR ( "[[b;cyan;]MEMORY]" ) );
		client->printf_P ( PSTR ( "Free Memory:  %d B" ), ESP.getFreeHeap() );
		client->printf_P ( PSTR ( "Sketch Size:  %s" ), formatBytes ( ESP.getSketchSize() ).c_str() );
		client->printf_P ( PSTR ( "Sketch Free:  %s" ), formatBytes ( ESP.getFreeSketchSpace() ).c_str() );
		client->printf_P ( PSTR ( "Flash Space:  %s" ), formatBytes ( ESP.getFlashChipRealSize() ).c_str() );
		client->printf_P ( PSTR ( "Flash Speed:  %d MHz\n " ), int ( ESP.getFlashChipSpeed() / 1000000 ) );

		client->printf_P ( PSTR ( "[[b;cyan;]NETWORK]" ) );
		client->printf_P ( PSTR ( "SoftAP SSID: %s" ), ssid );
		client->printf_P ( PSTR ( "SoftAP  MAC: %02X:%02X:%02X:%02X:%02X:%02X\nSoftAP   IP: " IPSTR "\n " ),
						   MAC2STR ( mac ),
						   IP2STR ( ip )
						 ) ;

		client_status ( client );
	}
	else if ( ( l >= 4 && !strncasecmp_P ( msg, PSTR ( "beep " ), 5 ) ) || !strcasecmp_P ( msg, PSTR ( "beep" ) ) )
	{
		if (strstr(msg, "rr"))
		{
			dbg_printf ( "[[b;green;]NEVER GONNA GIVE YOU UP!]" );
			beep_rr( true );
		}
		else
		{
			int v = atoi ( &msg[5] );
			if ( v == 0 ) v = 50;
			dbg_printf ( "[[b;yellow;]BEEP!] %dms" , v );
			beep ( v );
		}

	}
	else if ( ( l > 5 && !strncasecmp_P ( msg, PSTR ( "ssid " ), 5 ) )  )
	{
		sprintf ( ssid, "%s", &msg[5] );
		dbg_printf ( "[[b;yellow;]Changing SSID:] %s" , ssid );
		beep ( 1000 );
		setupAP();
	}
	else if ( !strcasecmp_P ( msg, PSTR ( "count" ) ) )
	{
		client->printf_P ( PSTR ( "[[b;yellow;]Rick Roll Count]: %d" ) , rrcount );
	}
	else if ( ( *msg == '?' || !strcasecmp_P ( msg, PSTR ( "help" ) ) ) )
	{
		client->printf_P ( PSTR ( HELP_TEXT ) );
	}
	else if ( !strcasecmp_P ( msg, PSTR ( "reboot" ) ) )
	{
		ws.closeAll() ;
		delay ( 1000 ) ;
		ESP.restart() ;
	}

	dbg_printf ( "WS[%d]: %s", client->id(), msg );
}

void client_status ( AsyncWebSocketClient * client )
{
	struct station_info *station = wifi_softap_get_station_info();
	uint8_t client_count = wifi_softap_get_station_num();
	struct ip_addr *ip;
	client->printf_P ( PSTR ( "[[b;yellow;]Connected Client(s)]: %d" ),
					   client_count
					 ) ;
	int i = 1;

	while ( station != NULL )
	{
		ip = &station->ip;
		client->printf_P ( PSTR ( "%d: MAC: %02X:%02X:%02X:%02X:%02X:%02X\n    IP: " IPSTR ),
						   i,
						   MAC2STR ( station->bssid ),
						   IP2STR ( ip->addr )
						 ) ;
		i++;
		station = STAILQ_NEXT ( station, next );
	}

	wifi_softap_free_station_info();
}
