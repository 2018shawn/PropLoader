#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <limits.h>
#include <ctype.h>

#include <iostream>

#include "loadelf.h"
#include "propimage.h"
#include "packet.h"
#include "loader.h"
#include "serialpropconnection.h"
#include "wifipropconnection.h"
#include "config.h"

/* port prefix */
#if defined(CYGWIN) || defined(WIN32) || defined(MINGW)
  #define PORT_PREFIX ""
#elif defined(LINUX)
  #ifdef RASPBERRY_PI
    #define PORT_PREFIX "ttyAMA"
  #else
    #define PORT_PREFIX "ttyUSB"
  #endif
#elif defined(MACOSX)
  #define PORT_PREFIX "cu.usbserial-"
#else
  #define PORT_PREFIX ""
#endif

static void usage(const char *progname)
{
printf("\
usage: %s [options] [<file>]\n\
\n\
options:\n\
    -b <type>       select target board and subtype (default is 'default:default')\n\
    -D var=value    define a board configuration variable\n\
    -e              program eeprom (and halt, unless combined with -r)\n\
    -f <file>       write a file to the SD card\n\
    -i <ip-addr>    IP address of the Parallax Wi-Fi module\n\
    -I <path>       add a directory to the include path\n\
    -n <name>       set the name of a Parallax Wi-Fi module\n\
    -p <port>       serial port\n\
    -P              show all serial ports\n\
    -r              run program after downloading (useful with -e)\n\
    -R              reset the Propeller\n\
    -s              do a serial download\n\
    -t              enter terminal mode after the load is complete\n\
    -T              enter pst-compatible terminal mode after the load is complete\n\
    -v              enable verbose debugging output\n\
    -W              show all discovered wifi modules\n\
    -?              display a usage message and exit\n\
\n\
file:               binary file to load (.elf or .binary)\n\
\n\
Target board type can be either a single identifier like 'propboe' in which case the subtype\n\
defaults to 'default' or it can be of the form <type>:<subtype> like 'c3:ram'.\n\
\n\
Module names should only include the characters A-Z, a-z, 0-9, or '-' and should not begin or\n\
end with a '-'. They must also be less than 32 characters long.\n\
\n\
Variables that can be set with -D are:\n\
  clkfreq clkmode baudrate reset rxpin txpin tvpin\n\
  sdspi-do sdspi-clk sdspi-di sdspi-cs\n\
  sdspi-clr sdspi-inc sdspi-start sdspi-width spdspi-addr\n\
  sdspi-config1 sdspi-config2\n\
\n\
Value expressions for -D can include:\n\
  rcfast rcslow xinput xtal1 xtal2 xtal3 pll1x pll2x pll4x pll8x pll16x k m mhz true false\n\
  an integer or two operands with a binary operator + - * / %% & | or unary + or -\n\
  or a parenthesized expression.\n", progname);
    exit(1);
}

int verbose = false;

static void ShowPorts(const char *prefix, bool check);
static void ShowWiFiModules(bool check);
static int WriteFileToSDCard(BoardConfig *config, PropConnection *connection, const char *path, const char *target);
static int LoadSDHelper(BoardConfig *config, PropConnection *connection);

int main(int argc, char *argv[])
{
    BoardConfig *config, *configSettings;
    bool done = false;
    bool reset = false;
    bool showPorts = false;
    bool showModules = false;
    bool terminalMode = false;
    bool pstTerminalMode = false;
    const char *board = NULL;
    const char *subtype = NULL;
    const char *ipaddr = NULL;
    const char *port = NULL;
    const char *name = NULL;
    const char *file = NULL;
    int loadType = ltShutdown;
    bool useSerial = false;
    bool writeFile = false;
    SerialPropConnection *serialConnection = NULL;
    WiFiPropConnection *wifiConnection = NULL;
    PropConnection *connection;
    Loader loader;
    const char *p;
    int sts, i;
    
    /* setup a configuration to collect command line -D settings */
    configSettings = NewBoardConfig(NULL, "");

    /* get the arguments */
    for(i = 1; i < argc; ++i) {

        /* handle switches */
        if(argv[i][0] == '-') {
            switch(argv[i][1]) {
            case 'b':   // select a target board
                if (argv[i][2])
                    board = &argv[i][2];
                else if (++i < argc)
                    board = argv[i];
                else
                    usage(argv[0]);
                break;
            case 'D':
                if(argv[i][2])
                    p = &argv[i][2];
                else if(++i < argc)
                    p = argv[i];
                else
                    usage(argv[0]);
                {
                    const char *p2;
                    char var[128];
                    if ((p2 = strchr(p, '=')) == NULL)
                        usage(argv[0]);
                    if (p2 - p > (int)sizeof(var) - 1) {
                        printf("error: variable name too long");
                        return 1;
                    }
                    strncpy(var, p, p2 - p);
                    var[p2 - p] = '\0';
                    SetConfigField(configSettings, var, p2 + 1);
                }
                break;
            case 'e':   // program eeprom
                loadType |= ltDownloadAndProgram;
                break;
            case 'f':   // write a file to the SD card
                if (argv[i][2])
                    file = &argv[i][2];
                else if (++i < argc)
                    file = argv[i];
                else
                    usage(argv[0]);
                writeFile = true;
                break;
            case 'i':   // set the ip address
                if (argv[i][2])
                    ipaddr = &argv[i][2];
                else if (++i < argc)
                    ipaddr = argv[i];
                else
                    usage(argv[0]);
                useSerial = false;
                break;
            case 'I':   // add a directory to the .cfg include path
                if(argv[i][2])
                    p = &argv[i][2];
                else if(++i < argc)
                    p = argv[i];
                else
                    usage(argv[0]);
                xbAddPath(p);
                break;
            case 'n':   // name a wifi module
                if (argv[i][2])
                    name = &argv[i][2];
                else if (++i < argc)
                    name = argv[i];
                else
                    usage(argv[0]);
                done = true;
                break;
            case 'p':   // select a serial port
                if (argv[i][2])
                    port = &argv[i][2];
                else if (++i < argc)
                    port = argv[i];
                else
                    usage(argv[0]);
#if defined(CYGWIN) || defined(WIN32) || defined(LINUX)
                if (isdigit((int)port[0])) {
#if defined(CYGWIN) || defined(WIN32)
                    static char buf[10];
                    sprintf(buf, "COM%d", atoi(port));
                    port = buf;
#endif
#if defined(LINUX)
                    static char buf[64];
                    sprintf(buf, "/dev/%s%d", PORT_PREFIX, atoi(port));
                    port = buf;
#endif
                }
#endif
#if defined(MACOSX)
                if (port[0] != '/') {
                    static char buf[64];
                    sprintf(buf, "/dev/%s-%s", PORT_PREFIX, port);
                    port = buf;
                }
#endif
                useSerial = true;
                break;
            case 'P':   // show serial ports
                showPorts = true;
                break;
            case 'r':   // run program after loading
                loadType |= ltDownloadAndRun;
                break;
            case 'R':   // reset the Propeller
                reset = true;
                done = true;
                break;
            case 's':   // use the serial loader instead of the wifi loader
                useSerial = true;
                break;
            case 't':   // enter terminal emulator mode after loading
                terminalMode = true;
                pstTerminalMode = false;
                break;
            case 'T':   // enter pst-compatible terminal emulator mode after loading
                terminalMode = true;
                pstTerminalMode = true;
                break;
            case 'v':   // enable verbose debugging output
                verbose = true;
                break;
            case 'W':   // show wifi modules
                showModules = true;
                break;
            default:
                usage(argv[0]);
                break;
            }
        }
        
        /* remember the file to load */
        else {
            if (file)
                usage(argv[0]);
            file = argv[i];
        }
    }

    /* show ports if requested */
    if (showPorts) {
        ShowPorts(PORT_PREFIX, false);
        done = true;
    }
    
    /* show modules if requested */
    if (showModules) {
        ShowWiFiModules(true);
        done = true;
    }
       
/*
1) look in the directory specified by the -I command line option (added above)
2) look in the directory where the elf file resides
3) look in the directory pointed to by the environment variable PROPELLER_ELF_LOAD
4) look in the directory where the loader executable resides if possible
5) look in /opt/parallax/propeller-load
*/

    /* finish the include path */
    if (file)
        xbAddFilePath(file);
    xbAddEnvironmentPath("PROPELLER_LOAD_PATH");
    xbAddProgramPath(argv);
#if defined(LINUX) || defined(MACOSX) || defined(CYGWIN)
    xbAddPath("/opt/parallax/propeller-load");
#endif
    
    /* parse the board option */
    char boardBuffer[128];
    if (board) {
    
        /* split the board type from the subtype */
        if ((p = strchr(board, ':')) != NULL) {
            if (p - board >= (int)sizeof(boardBuffer)) {
                printf("error: board type name too long\n");
                return 1;
            }
            strncpy(boardBuffer, board, p - board);
            boardBuffer[p - board] = '\0';
            board = boardBuffer;
            subtype = p + 1;
        }
        
        /* no subtype */
        else
            subtype = DEF_SUBTYPE;
    }
    
    else {
        board = DEF_BOARD;
        subtype = DEF_SUBTYPE;
    }

    /* setup for the selected board */
    if (!(config = ParseConfigurationFile(board))) {
        printf("error: can't find board configuration '%s'\n", board);
        return 1;
    }
    
    /* select the subtype */
    if (subtype) {
        if (!(config = GetConfigSubtype(config, subtype))) {
            printf("error: can't find board configuration subtype '%s'\n", subtype);
            return 1;
        }
    }
    
    /* override with any command line settings */
    config = MergeConfigs(config, configSettings);
    
    /* make sure a file to load was specified */
    if (!done && !reset && !file && !terminalMode)
        usage(argv[0]);
        
    /* check to see if a reset was requested or there is a file to load */
    if (!reset && !file && !name && !terminalMode)
        goto finish;

    /* default to 'download and run' if neither -e nor -r are specified */
    if (loadType == ltShutdown)
        loadType = ltDownloadAndRun;
        
    /* do a serial download */
    if (useSerial) {
        SerialInfo info; // needs to stay in scope as long as we're using port
        if (!(serialConnection = new SerialPropConnection)) {
            printf("error: insufficient memory\n");
            return 1;
        }
        if (!port) {
            SerialInfoList ports;
            if (SerialPropConnection::findPorts(PORT_PREFIX, true, ports) != 0) {
                printf("error: serial port discovery failed\n");
                return 1;
            }
            if (ports.size() == 0) {
                printf("error: no serial ports found\n");
                return 1;
            }
            info = ports.front();
            port = info.port();
        }
        if ((sts = serialConnection->open(port)) != 0) {
            printf("error: loader initialization failed: %d\n", sts);
            return 1;
        }
        connection = serialConnection;
    }
    
    /* do a wifi download */
    else {
        if (!(wifiConnection = new WiFiPropConnection)) {
            printf("error: insufficient memory\n");
            return 1;
        }
        if (!ipaddr) {
            WiFiInfoList addrs;
            if (WiFiPropConnection::findModules(false, addrs, 1) != 0) {
                printf("error: wifi module discovery failed\n");
                return 1;
            }
            if (addrs.size() == 0) {
                printf("error: no wifi module found\n");
                return 1;
            }
            ipaddr = addrs.front().address();
        }
        if ((sts = wifiConnection->setAddress(ipaddr)) != 0) {
            printf("error: setAddress failed: %d\n", sts);
            return 1;
        }
        connection = wifiConnection;
    }
    
    /* setup the baud rates */
    int baudRate;
    if (GetNumericConfigField(config, "loader-baud-rate", &baudRate))
        connection->setLoaderBaudRate(baudRate);
    if (GetNumericConfigField(config, "fast-loader-baud-rate", &baudRate))
        connection->setFastLoaderBaudRate(baudRate);
    if (GetNumericConfigField(config, "program-baud-rate", &baudRate))
        connection->setProgramBaudRate(baudRate);
        
    /* reset the Propeller */
    if (reset) {
        if (connection->generateResetSignal() != 0) {
            printf("error: failed to reset Propeller\n");
            return 1;
        }
    }
    
    /* set the wifi module name */
    if (name) {
        if (!wifiConnection) {
            printf("error: -n can only be used to name wifi modules\n");
            return 1;
        }
        
#define isAllowed(ch)   (isupper(ch) || islower(ch) || isdigit(ch) || (ch) == '-')

        char cleanName[32], *p;
        
        /* remove leading spaces or hyphens */
        p = cleanName;
        while (*name && (isspace(*name) || *name == '-'))
            ++name;
        
        /* copy the rest of the name */
        bool inStringOfSpaces = false;
        while (*name != '\0' && p < &cleanName[sizeof(cleanName) - 1]) {
            if (isspace(*name)) {
                if (!inStringOfSpaces)
                    *p++ = '-';
                inStringOfSpaces = true;
            }
            else if (isAllowed(*name)) {
                *p++ = *name;
                inStringOfSpaces = false;
            }
            ++name;
        }
            
        /* remove trailing spaces or hyphens */
        while (p > cleanName && (isspace(p[-1]) || p[-1] == '-'))
            --p;
            
        /* terminate the clean name */
        *p = '\0';
        
        /* if we deleted every character then this is an invalid name */
        if (!cleanName[0]) {
            printf("error: invalid name\n");
            return 1;
        }
        
        /* show the clean name if it is different from what the user requested */
        if (strcmp(name, cleanName) != 0)
            printf("Setting module name to '%s'\n", cleanName);
            
        if (wifiConnection->setName(cleanName) != 0) {
            printf("error: failed to set module name\n");
            return 1;
        }
    }
    
    /* write a file to the SD card */
    if (writeFile) {
        printf("Writing '%s' to the SD card\n", file);
        if (WriteFileToSDCard(config, connection, file, file) != 0) {
            printf("error: writing '%s'\n", file);
            return 1;
        }
    }
    
    /* load a file */
    else if (file) {
        printf("Loading '%s'\n", file);
        loader.setConnection(connection);
        if (file && (sts = loader.fastLoadFile(file, (LoadType)loadType)) != 0) {
            printf("error: load failed: %d\n", sts);
            return 1;
        }
    }
    
    /* set the baud rate used by the program */
    if (connection->setBaudRate(connection->programBaudRate()) != 0) {
        printf("error: failed to set baud rate\n");
        return 1;
    }
    
    /* enter terminal mode */
    if (terminalMode) {
        printf("[ Entering terminal mode. Type ESC or Control-C to exit. ]\n");
        connection->terminal(false, pstTerminalMode);
    }
    
    /* disconnect from the target */
    connection->disconnect();
    
finish:
    /* return successfully */
    return 0;
}

static void ShowPorts(const char *prefix, bool check)
{
    SerialInfoList ports;
    if (SerialPropConnection::findPorts(prefix, check, ports) == 0) {
        SerialInfoList::iterator i = ports.begin();
        while (i != ports.end()) {
            std::cout << i->port() << std::endl;
            ++i;
        }
    }
}

static void ShowWiFiModules(bool show)
{
    WiFiInfoList modules;
    WiFiPropConnection::findModules(true, modules);
}

int Error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("error: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    return -1;
}

#define TYPE_FILE_WRITE     0
#define TYPE_DATA           1
#define TYPE_EOF            2

static int WriteFileToSDCard(BoardConfig *config, PropConnection *connection, const char *path, const char *target)
{
    PacketDriver packetDriver(*connection);
    uint8_t buf[PKTMAXLEN];
    size_t size, remaining, cnt;
    FILE *fp;

    /* open the file */
    printf("Opening '%s'\n", path);
    if ((fp = fopen(path, "rb")) == NULL)
        return Error("can't open %s", path);

    if (!target) {
        if (!(target = strrchr(path, '/')))
            target = path;
        else
            ++target; // skip past the slash
    }
    printf("Target is '%s'\n", target);

    fseek(fp, 0, SEEK_END);
    size = remaining = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    printf("Loading SD helper\n");
    if (LoadSDHelper(config, connection) != 0) {
        fclose(fp);
        return Error("loading SD helper");
    }

    /* wait for the SD helper to complete initialization */
    printf("Waiting for SD helper to start\n");
    if (!packetDriver.waitForInitialAck())
        return Error("failed to connect to helper");

    printf("Starting file write\n");
    if (!packetDriver.sendPacket(TYPE_FILE_WRITE, (uint8_t *)target, strlen(target) + 1)) {
        fclose(fp);
        return Error("SendPacket FILE_WRITE failed");
    }

    printf("Loading '%s' to SD card\n", path);
    while ((cnt = fread(buf, 1, PKTMAXLEN, fp)) > 0) {
        printf("%ld bytes remaining             \r", remaining); fflush(stdout);
        if (!packetDriver.sendPacket(TYPE_DATA, buf, cnt)) {
            fclose(fp);
            return Error("SendPacket DATA failed");
        }
        remaining -= cnt;
    }
    printf("%ld bytes sent             \n", size);

    fclose(fp);

    if (!packetDriver.sendPacket(TYPE_EOF, (uint8_t *)"", 0))
        return Error("SendPacket EOF failed");

    /*
       We send two EOF packets for SD card writes.  The reason is that the EOF
       packet does actual work, and that work takes time.  The packet
       transmission protocol uses read-ahead buffering on the receiving end.
       Therefore, we need to make sure the first EOF packet was received and
       processed before resetting the Prop!
    */
    if (!packetDriver.sendPacket(TYPE_EOF, (uint8_t *)"", 0))
        return Error("Second SendPacket EOF failed");

    return 0;
}

extern "C" {
    extern uint8_t sd_helper_array[];
    extern int sd_helper_size;
}

/* DAT header in sd_helper.spin */
typedef struct {
    uint32_t baudrate;
    uint8_t rxpin;
    uint8_t txpin;
    uint8_t tvpin;
    uint8_t dopin;
    uint8_t clkpin;
    uint8_t dipin;
    uint8_t cspin;
    uint8_t select_address;
    uint32_t select_inc_mask;
    uint32_t select_mask;
} SDHelperDatHdr;

static int LoadSDHelper(BoardConfig *config, PropConnection *connection)
{
    Loader loader(connection);
    PropImage image(sd_helper_array, sd_helper_size);
    SpinHdr *hdr = (SpinHdr *)image.imageData();
    SpinObj *obj = (SpinObj *)(image.imageData() + hdr->pbase);
    SDHelperDatHdr *dat = (SDHelperDatHdr *)((uint8_t *)obj + (obj->pubcnt + obj->objcnt) * sizeof(uint32_t));
    int ivalue;

    /* patch SD helper */
    if (GetNumericConfigField(config, "clkfreq", &ivalue))
        hdr->clkfreq = ivalue;
    if (GetNumericConfigField(config, "clkmode", &ivalue))
        hdr->clkmode = ivalue;
    if (GetNumericConfigField(config, "baudrate", &ivalue))
        dat->baudrate = ivalue;
    if (GetNumericConfigField(config, "rxpin", &ivalue))
        dat->rxpin = ivalue;
    if (GetNumericConfigField(config, "txpin", &ivalue))
        dat->txpin = ivalue;
    if (GetNumericConfigField(config, "tvpin", &ivalue))
        dat->tvpin = ivalue;

    if (GetNumericConfigField(config, "sdspi-do", &ivalue))
        dat->dopin = ivalue;
    else
        return Error("missing sdspi-do pin configuration");

    if (GetNumericConfigField(config, "sdspi-clk", &ivalue))
        dat->clkpin = ivalue;
    else
        return Error("missing sdspi-clk pin configuration");

    if (GetNumericConfigField(config, "sdspi-di", &ivalue))
        dat->dipin = ivalue;
    else
        return Error("missing sdspi-di pin configuration");

    if (GetNumericConfigField(config, "sdspi-cs", &ivalue))
        dat->cspin = ivalue;
    else if (GetNumericConfigField(config, "sdspi-clr", &ivalue))
        dat->cspin = ivalue;
    else
        return Error("missing sdspi-cs or sdspi-clr pin configuration");

    if (GetNumericConfigField(config, "sdspi-sel", &ivalue))
        dat->select_inc_mask = ivalue;
    else if (GetNumericConfigField(config, "sdspi-inc", &ivalue))
        dat->select_inc_mask = 1 << ivalue;

    if (GetNumericConfigField(config, "sdspi-msk", &ivalue))
        dat->select_mask = ivalue;

    if (GetNumericConfigField(config, "sdspi-addr", &ivalue))
        dat->select_address = (uint8_t)ivalue;

    /* recompute the checksum */
    image.updateChecksum();

    /* load the SD helper program */
    if (loader.fastLoadImage(image.imageData(), image.imageSize(), ltDownloadAndRun) != 0)
        return Error("helper load failed");
        
    /* select the sd helper baud rate */
    connection->setBaudRate(dat->baudrate);

    return 0;
}


