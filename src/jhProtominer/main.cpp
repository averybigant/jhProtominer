#include "global.h"
#include <time.h>



// miner version string (for pool statistic)
#ifdef __WIN32__
char* minerVersionString = _strdup("jhProtominer for PTSPool.com");
#else
char* minerVersionString = _strdup("jhProtominer for PTSPool-Linux by averybigant");
#include <cstdarg>
#include <iostream>
#endif

minerSettings_t minerSettings = {0};

xptClient_t* xptClient = NULL;
CRITICAL_SECTION cs_xptClient;

volatile bool restarts[128];

typedef struct  
{
	char* workername;
	char* workerpass;
	char* host;
	sint32 port;
	sint32 numThreads;
	uint32 ptsMemoryMode;
	uint32 mode;
} commandlineInput_t;

commandlineInput_t commandlineInput;

void applog(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	char f[1024];
	int len;
	time_t rawtime;
	struct tm timeinfo;
	time(&rawtime);
	//localtime_s(&timeinfo, &rawtime);
	timeinfo = *localtime(&rawtime);
	len = strlen(fmt) + 13;
	//sprintf_s(f, "[%02d:%02d:%02d] %s\n", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, fmt);
	sprintf(f, "[%02d:%02d:%02d] %s\n", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, fmt);
	vfprintf(stderr, f, ap);
	va_end(ap);
}

struct  
{
	CRITICAL_SECTION cs_work;
	uint32	algorithm;
	// block data
	uint32	version;
	uint32	height;
	uint32	nBits;
	uint32	nTime;
	uint8	merkleRootOriginal[32]; // used to identify work
	uint8	prevBlockHash[32];
	uint8	target[32];
	uint8	targetShare[32];
	// extra nonce info
	uint8	coinBase1[1024];
	uint8	coinBase2[1024];
	uint16	coinBase1Size;
	uint16	coinBase2Size;
	// transaction hashes
	uint8	txHash[32*256];
	uint32	txHashCount;
}workDataSource;

uint32 uniqueMerkleSeedGenerator = 0;
uint32 miningStartTime = 0;

void jhProtominer_submitShare(minerProtosharesBlock_t* block)
{
	applog("Share found!");
	EnterCriticalSection(&cs_xptClient);
	if( xptClient == NULL )
	{
		applog("Share submission failed - No connection to server");
		LeaveCriticalSection(&cs_xptClient);
		return;
	}
	// submit block
	xptShareToSubmit_t* xptShare = (xptShareToSubmit_t*)malloc(sizeof(xptShareToSubmit_t));
	memset(xptShare, 0x00, sizeof(xptShareToSubmit_t));
	xptShare->algorithm = ALGORITHM_PROTOSHARES;
	xptShare->version = block->version;
	xptShare->nTime = block->nTime;
	xptShare->nonce = block->nonce;
	xptShare->nBits = block->nBits;
	xptShare->nBirthdayA = block->birthdayA;
	xptShare->nBirthdayB = block->birthdayB;
	memcpy(xptShare->prevBlockHash, block->prevBlockHash, 32);
	memcpy(xptShare->merkleRoot, block->merkleRoot, 32);
	memcpy(xptShare->merkleRootOriginal, block->merkleRootOriginal, 32);
	//userExtraNonceLength = min(userExtraNonceLength, 16);
	sint32 userExtraNonceLength = sizeof(uint32);
	uint8* userExtraNonceData = (uint8*)&block->uniqueMerkleSeed;
	xptShare->userExtraNonceLength = userExtraNonceLength;
	memcpy(xptShare->userExtraNonceData, userExtraNonceData, userExtraNonceLength);
	xptClient_foundShare(xptClient, xptShare);
	LeaveCriticalSection(&cs_xptClient);
}

int jhProtominer_minerThread(int threadIndex)
{
	while( true )
	{
		// local work data
		minerProtosharesBlock_t minerProtosharesBlock = {0};
		// has work?
		bool hasValidWork = false;
		EnterCriticalSection(&workDataSource.cs_work);
		if( workDataSource.height > 0 )
		{
			// get work data
			minerProtosharesBlock.version = workDataSource.version;
			//minerProtosharesBlock.nTime = workDataSource.nTime;
			minerProtosharesBlock.nTime = (uint32)time(NULL);
			minerProtosharesBlock.nBits = workDataSource.nBits;
			minerProtosharesBlock.nonce = 0;
			minerProtosharesBlock.height = workDataSource.height;
			memcpy(minerProtosharesBlock.merkleRootOriginal, workDataSource.merkleRootOriginal, 32);
			memcpy(minerProtosharesBlock.prevBlockHash, workDataSource.prevBlockHash, 32);
			memcpy(minerProtosharesBlock.targetShare, workDataSource.targetShare, 32);
			minerProtosharesBlock.uniqueMerkleSeed = uniqueMerkleSeedGenerator;
			uniqueMerkleSeedGenerator++;
			// generate merkle root transaction
			bitclient_generateTxHash(sizeof(uint32), (uint8*)&minerProtosharesBlock.uniqueMerkleSeed, workDataSource.coinBase1Size, workDataSource.coinBase1, workDataSource.coinBase2Size, workDataSource.coinBase2, workDataSource.txHash);
			bitclient_calculateMerkleRoot(workDataSource.txHash, workDataSource.txHashCount+1, minerProtosharesBlock.merkleRoot);
			hasValidWork = true;
		}
		LeaveCriticalSection(&workDataSource.cs_work);
		if( hasValidWork == false )
		{
			Sleep(1);
			continue;
		}
		restarts[threadIndex] = false;
		// valid work data present, start mining
		switch( minerSettings.protoshareMemoryMode )
		{
		case PROTOSHARE_MEM_1024:
			protoshares_process_1024(&minerProtosharesBlock, &restarts[threadIndex]);
			break;
		case PROTOSHARE_MEM_512:
			protoshares_process_512(&minerProtosharesBlock, &restarts[threadIndex]);
			break;
		case PROTOSHARE_MEM_256:
			protoshares_process_256(&minerProtosharesBlock, &restarts[threadIndex]);
			break;
		case PROTOSHARE_MEM_128:
			protoshares_process_128(&minerProtosharesBlock, &restarts[threadIndex]);
			break;
		case PROTOSHARE_MEM_32:
			protoshares_process_32(&minerProtosharesBlock, &restarts[threadIndex]);
			break;
		case PROTOSHARE_MEM_8:
			protoshares_process_8(&minerProtosharesBlock, &restarts[threadIndex]);
			break;
		default:
			applog("Unknown memory mode");
			Sleep(5000);
			break;
		}
	}
	return 0;
}


/*
 * Reads data from the xpt connection state and writes it to the universal workDataSource struct
 */
void jhProtominer_getWorkFromXPTConnection(xptClient_t* xptClient)
{
	EnterCriticalSection(&workDataSource.cs_work);
	workDataSource.height = xptClient->blockWorkInfo.height;
	workDataSource.version = xptClient->blockWorkInfo.version;
	//uint32 timeBias = time(NULL) - xptClient->blockWorkInfo.timeWork;
	workDataSource.nTime = xptClient->blockWorkInfo.nTime;// + timeBias;
	workDataSource.nBits = xptClient->blockWorkInfo.nBits;
	memcpy(workDataSource.merkleRootOriginal, xptClient->blockWorkInfo.merkleRoot, 32);
	memcpy(workDataSource.prevBlockHash, xptClient->blockWorkInfo.prevBlockHash, 32);
	memcpy(workDataSource.target, xptClient->blockWorkInfo.target, 32);
	memcpy(workDataSource.targetShare, xptClient->blockWorkInfo.targetShare, 32);

	workDataSource.coinBase1Size = xptClient->blockWorkInfo.coinBase1Size;
	workDataSource.coinBase2Size = xptClient->blockWorkInfo.coinBase2Size;
	memcpy(workDataSource.coinBase1, xptClient->blockWorkInfo.coinBase1, xptClient->blockWorkInfo.coinBase1Size);
	memcpy(workDataSource.coinBase2, xptClient->blockWorkInfo.coinBase2, xptClient->blockWorkInfo.coinBase2Size);

	// get hashes
	if( xptClient->blockWorkInfo.txHashCount >= 256 )
	{
		applog("Too many transaction hashes"); 
		workDataSource.txHashCount = 0;
	}
	else
		workDataSource.txHashCount = xptClient->blockWorkInfo.txHashCount;
	for(uint32 i=0; i<xptClient->blockWorkInfo.txHashCount; i++)
		memcpy(workDataSource.txHash+32*(i+1), xptClient->blockWorkInfo.txHashes+32*i, 32);
	//// generate unique work from custom extra nonce
	//uint32 userExtraNonce = xpc->coinbaseSeed;
	//xpc->coinbaseSeed++;
	//bitclient_generateTxHash(sizeof(uint32), (uint8*)&userExtraNonce, xpc->xptClient->blockWorkInfo.coinBase1Size, xpc->xptClient->blockWorkInfo.coinBase1, xpc->xptClient->blockWorkInfo.coinBase2Size, xpc->xptClient->blockWorkInfo.coinBase2, xpc->xptClient->blockWorkInfo.txHashes);
	//bitclient_calculateMerkleRoot(xpc->xptClient->blockWorkInfo.txHashes, xpc->xptClient->blockWorkInfo.txHashCount+1, workData->merkleRoot);
	//workData->errorCode = 0;
	//workData->shouldTryAgain = false;
	//xpc->timeCacheClear = GetTickCount() + CACHE_TIME_WORKER;
	//xptProxyWorkCache_add(workData->merkleRoot, workData->merkleRootOriginal, sizeof(uint32), (uint8*)&userExtraNonce);
	LeaveCriticalSection(&workDataSource.cs_work);
}

void jhProtominer_xptQueryWorkLoop()
{
	xptClient = NULL;
	uint32 timerPrintDetails = GetTickCount() + 20000;
	while( true )
	{
		uint32 currentTick = GetTickCount();
		if( currentTick >= timerPrintDetails )
		{
			// print details only when connected
			if( xptClient )
			{
				uint32 passedSeconds = time(NULL) - miningStartTime;
				double collisionsPerMinute = 0.0;
				if( passedSeconds > 5 )
				{
					collisionsPerMinute = (double)totalCollisionCount / (double)passedSeconds * 60.0;
				}
				applog("collisions/min: %.4lf Shares total: %d", collisionsPerMinute, totalShareCount);
			}
			timerPrintDetails = currentTick + 20000;
		}
		// check stats
		if( xptClient )
		{
			EnterCriticalSection(&cs_xptClient);
			xptClient_process(xptClient);
			if( xptClient->disconnected )
			{
				// mark work as invalid
				EnterCriticalSection(&workDataSource.cs_work);
				workDataSource.height = 0;
				LeaveCriticalSection(&workDataSource.cs_work);
				// we lost connection :(
				applog("Connection to server lost - Reconnect in 20 seconds");
				xptClient_free(xptClient);
				xptClient = NULL;
				LeaveCriticalSection(&cs_xptClient);
				Sleep(20000);
			}
			else
			{
				// is protoshare algorithm?
				if( xptClient->clientState == XPT_CLIENT_STATE_LOGGED_IN && xptClient->algorithm != ALGORITHM_PROTOSHARES )
				{
					applog("The miner is configured to use a different algorithm.");
					applog("Make sure you miner login details are correct");
					// force disconnect
					xptClient_free(xptClient);
					xptClient = NULL;
				}
				else if( xptClient->blockWorkInfo.height != workDataSource.height )
				{
					// update work
					jhProtominer_getWorkFromXPTConnection(xptClient);
					if (totalCollisionCount) {
						char *hex = "0123456789abcdef";
						char prevblk[65];
						for (int i = 0; i < 32; i++) {
							prevblk[i * 2] = hex[(unsigned int)xptClient->blockWorkInfo.prevBlockHash[31 - i] / 16];
							prevblk[i * 2 + 1] = hex[(unsigned int)xptClient->blockWorkInfo.prevBlockHash[31 - i] % 16];
						}
						prevblk[64] = '\0';
						applog("New block: %d %s", xptClient->blockWorkInfo.height - 1, prevblk);
						for (int i = 0; i < commandlineInput.numThreads; i++) {
							restarts[i] = true;
						}
					}
					totalCollisionCount += commandlineInput.numThreads;
				}
				LeaveCriticalSection(&cs_xptClient);
				Sleep(1);
			}
		}
		else
		{
			// initiate new connection
			EnterCriticalSection(&cs_xptClient);
			xptClient = xptClient_connect(&minerSettings.requestTarget, 0);
			if( xptClient == NULL )
			{
				LeaveCriticalSection(&cs_xptClient);
				applog("Connection attempt failed, retry in 20 seconds");
				Sleep(20000);
			}
			else
			{
				LeaveCriticalSection(&cs_xptClient);
				applog("Connected to server using x.pushthrough(xpt) protocol");
				miningStartTime = (uint32)time(NULL);
				totalCollisionCount = 0;
			}
		}
	}
}

void jhProtominer_printHelp()
{
	puts("Usage: jhProtominer.exe [options]");
	puts("Options:");
	puts("   -o, -O                        The miner will connect to this url");
	puts("                                     You can specifiy an port after the url using -o url:port");
	puts("   -u                            The username (workername) used for login");
	puts("   -p                            The password used for login");
	puts("   -t <num>                      The number of threads for mining (default 4)");
	puts("                                 For most efficient mining, set to number of CPU cores");
	puts("   -m<amount>                    Defines how many megabytes of memory are used per thread.");
	puts("                                 Default is 256mb, allowed constants are:");
	puts("                                 -m1024 -m512 -m256 -m128 -m32 -m8");
	puts("Example usage:");
	puts("   jhProtominer.exe -o 112.124.13.238:28988 -u PpXRMhz5dDHtFYpbDTpiAMJaVarMUJURT6 -p x -t 4");
}

void jhProtominer_parseCommandline(int argc, char **argv)
{
	sint32 cIdx = 1;
	while( cIdx < argc )
	{
		char* argument = argv[cIdx];
		cIdx++;
		if( memcmp(argument, "-o", 3)==0 || memcmp(argument, "-O", 3)==0 )
		{
			// -o
			if( cIdx >= argc )
			{
				applog("Missing URL after -o option");
				exit(0);
			}
			if( strstr(argv[cIdx], "http://") )
				commandlineInput.host = _strdup(strstr(argv[cIdx], "http://")+7);
			else
				commandlineInput.host = _strdup(argv[cIdx]);
			char* portStr = strstr(commandlineInput.host, ":");
			if( portStr )
			{
				*portStr = '\0';
				commandlineInput.port = atoi(portStr+1);
			}
			cIdx++;
		}
		else if( memcmp(argument, "-u", 3)==0 )
		{
			// -u
			if( cIdx >= argc )
			{
				applog("Missing username/workername after -u option");
				exit(0);
			}
			commandlineInput.workername = _strdup(argv[cIdx]);
			cIdx++;
		}
		else if( memcmp(argument, "-p", 3)==0 )
		{
			// -p
			if( cIdx >= argc )
			{
				applog("Missing password after -p option");
				exit(0);
			}
			commandlineInput.workerpass = _strdup(argv[cIdx]);
			cIdx++;
		}
		else if( memcmp(argument, "-t", 3)==0 )
		{
			// -t
			if( cIdx >= argc )
			{
				applog("Missing thread number after -t option");
				exit(0);
			}
			commandlineInput.numThreads = atoi(argv[cIdx]);
			if( commandlineInput.numThreads < 1 || commandlineInput.numThreads > 128 )
			{
				applog("-t parameter out of range");
				exit(0);
			}
			cIdx++;
		}
		else if( memcmp(argument, "-m1024", 7)==0 )
		{
			commandlineInput.ptsMemoryMode = PROTOSHARE_MEM_1024;
		}
		else if( memcmp(argument, "-m512", 6)==0 )
		{
			commandlineInput.ptsMemoryMode = PROTOSHARE_MEM_512;
		}
		else if( memcmp(argument, "-m256", 6)==0 )
		{
			commandlineInput.ptsMemoryMode = PROTOSHARE_MEM_256;
		}
		else if( memcmp(argument, "-m128", 6)==0 )
		{
			commandlineInput.ptsMemoryMode = PROTOSHARE_MEM_128;
		}
		else if( memcmp(argument, "-m32", 5)==0 )
		{
			commandlineInput.ptsMemoryMode = PROTOSHARE_MEM_32;
		}
		else if( memcmp(argument, "-m8", 4)==0 )
		{
			commandlineInput.ptsMemoryMode = PROTOSHARE_MEM_8;
		}
		else if( memcmp(argument, "-help", 6)==0 || memcmp(argument, "--help", 7)==0 )
		{
			jhProtominer_printHelp();
			exit(0);
		}
		else
		{
			applog("'%s' is an unknown option.\nType jhPrimeminer.exe --help for more info", argument); 
			exit(-1);
		}
	}
	if( argc <= 1 )
	{
		jhProtominer_printHelp();
		exit(0);
	}
}


int main(int argc, char** argv)
{
	commandlineInput.host = "112.124.13.238";
	commandlineInput.port = 28988;
	commandlineInput.workername = "PpXRMhz5dDHtFYpbDTpiAMJaVarMUJURT6";
	commandlineInput.workerpass = "x";

	commandlineInput.ptsMemoryMode = PROTOSHARE_MEM_256;
	SYSTEM_INFO sysinfo;
	GetSystemInfo( &sysinfo );
	commandlineInput.numThreads = sysinfo.dwNumberOfProcessors;
	commandlineInput.numThreads = std::min(std::max(commandlineInput.numThreads, 1), 4);
	jhProtominer_parseCommandline(argc, argv);
	minerSettings.protoshareMemoryMode = commandlineInput.ptsMemoryMode;
	applog("Launching miner...");
	uint32 mbTable[] = {1024,512,256,128,32,8};
	size_t mmode = (size_t)commandlineInput.ptsMemoryMode;
	applog("Using %d megabytes of memory per thread", mbTable[std::min(mmode,(sizeof(mbTable)/sizeof(mbTable[0])))]);
	applog("Using %d threads", commandlineInput.numThreads);
	// set priority to below normal
	SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
	// init winsock
	WSADATA wsa;
	WSAStartup(MAKEWORD(2,2),&wsa);
	// get IP of pool url (default ypool.net)
	char* poolURL = commandlineInput.host;//"ypool.net";
	hostent* hostInfo = gethostbyname(poolURL);
	if( hostInfo == NULL )
	{
		applog("Cannot resolve '%s'. Is it a valid URL?", poolURL);
		exit(-1);
	}
	void** ipListPtr = (void**)hostInfo->h_addr_list;
	uint32 ip = 0xFFFFFFFF;
	if( ipListPtr[0] )
	{
		ip = *(uint32*)ipListPtr[0];
	}
	char* ipText = (char*)malloc(32);
	sprintf(ipText, "%d.%d.%d.%d", ((ip>>0)&0xFF), ((ip>>8)&0xFF), ((ip>>16)&0xFF), ((ip>>24)&0xFF));
	// init work source
	InitializeCriticalSection(&workDataSource.cs_work);
	InitializeCriticalSection(&cs_xptClient);
	// setup connection info
	minerSettings.requestTarget.ip = ipText;
	minerSettings.requestTarget.port = commandlineInput.port;
	minerSettings.requestTarget.authUser = commandlineInput.workername;//"jh00.pts_1";
	minerSettings.requestTarget.authPass = commandlineInput.workerpass;//"x";
	// start miner threads
	for(uint32 i=0; i<commandlineInput.numThreads; i++)
		CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)jhProtominer_minerThread, (LPVOID)0, 0, NULL);
	/*CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)jhProtominer_minerThread, (LPVOID)0, 0, NULL);
	CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)jhProtominer_minerThread, (LPVOID)0, 0, NULL);
	CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)jhProtominer_minerThread, (LPVOID)0, 0, NULL);*/
	// enter work management loop
	jhProtominer_xptQueryWorkLoop();
	return 0;
}
