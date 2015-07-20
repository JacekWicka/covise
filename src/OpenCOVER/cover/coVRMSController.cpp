/* This file is part of COVISE.

   You can use it under the terms of the GNU Lesser General Public License
   version 2.1 or later, see lgpl-2.1.txt.

 * License: LGPL 2+ */

#include <util/common.h>

#include <config/CoviseConfig.h>
#include <config/coConfigConstants.h>
#include <net/covise_connect.h>
#include <net/covise_socket.h>
#include <net/covise_host.h>
#include "coVRSlave.h"
#include "coVRPluginSupport.h"
#include "coVRCommunication.h"
#include "coVRNavigationManager.h"
#include "coVRFileManager.h"
#include "VRViewer.h"
#include "OpenCOVER.h"
#include "coHud.h"
#include "coClusterStat.h"
#include <vrbclient/VRBClient.h>
#include "coVRConfig.h"

#ifdef HAS_MPI
#include <mpi.h>
//#define MPI_BCAST
#ifndef CO_MPI_SEND
#define CO_MPI_SEND MPI_Ssend
#endif
#endif

using namespace covise;
using namespace opencover;

#define RINGBUFLEN 200
#ifdef __linux__
#include <linux/ppdev.h>
#endif

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#define NOMCAST
#else
#include <sys/ioctl.h>
#endif

#undef DOTIMING
#ifdef DOTIMING
#include <util/coTimer.h>
#else
#define MARK0(a)
#define MARK1(a, b)
#endif

#if !defined(NOMCAST) && defined(HAVE_NORM)
#include "rel_mcast.h"
#endif

#include <errno.h>
#include "coVRMSController.h"

#ifdef DEBUG_MESSAGES
int debugMessageCounter;
bool debugMessagesCheck;
#endif

coVRMSController *coVRMSController::s_singleton = NULL;

coVRMSController::SlaveData::SlaveData(int n)
    : data(coVRMSController::instance()->numSlaves)
    , n(n)
{
    for (int ctr = 0; ctr < coVRMSController::instance()->numSlaves; ++ctr)
    {
        this->data[ctr] = malloc(n);
        memset(this->data[ctr], 0, 1);
    }
}

coVRMSController::SlaveData::~SlaveData()
{
    for (size_t ctr = 0; ctr < this->data.size(); ++ctr)
    {
        free(this->data[ctr]);
    }
}

coVRMSController *coVRMSController::instance()
{
    assert(s_singleton);
    return s_singleton;
}

coVRMSController::coVRMSController(bool forceMpi, int AmyID, const char *addr, int port)
    : master(true)
    , slave(false)
    , myID(0)
    , socket(0)
    , socketDraw(0)
#ifdef HAS_MPI
    , appComm(MPI_COMM_WORLD)
    , drawComm(MPI_COMM_WORLD)
#endif
    , heartBeatCounter(0)
    , heartBeatCounterDraw(0)

{
    assert(!s_singleton);
    s_singleton = this;

    MARK0("coVRMSController::coVRMSController");
    if (AmyID >= 0)
    {
        myID = AmyID;
    }

    if (coVRConfig::instance()->debugLevel(2))
        fprintf(stderr, "\nnew coVRMSController\n");
#ifdef DEBUG_MESSAGES
    debugMessageCounter = 0;
    debugMessagesCheck = true;
#endif
    syncMode = SYNC_TCP;

    drawStatistics = coCoviseConfig::isOn("COVER.MultiPC.Statistics", false);
    //   cover->setBuiltInFunctionState("CLUSTER_STATISTICS",drawStatistics);

    // Multicast settings
    multicastDebugLevel = coCoviseConfig::getInt("COVER.MultiPC.Multicast.debugLevel", 0);
    multicastAddress = coCoviseConfig::getEntry("COVER.MultiPC.Multicast.mcastAddr");
    multicastPort = coCoviseConfig::getInt("COVER.MultiPC.Multicast.mcastPort", 23232);
    multicastInterface = coCoviseConfig::getEntry("COVER.MultiPC.Multicast.mcastIface");
    multicastMTU = coCoviseConfig::getInt("COVER.MultiPC.Multicast.mtu", 1500);
    multicastTTL = coCoviseConfig::getInt("COVER.MultiPC.Multicast.ttl", 1);
    multicastLoop = coCoviseConfig::isOn("COVER.MultiPC.Multicast.lback", false);
    multicastBufferSpace = coCoviseConfig::getInt("COVER.MultiPC.Multicast.bufferSpace", 1000000);
    multicastBlockSize = coCoviseConfig::getInt("COVER.MultiPC.Multicast.blockSize", 4);
    multicastNumParity = coCoviseConfig::getInt("COVER.MultiPC.Multicast.numParity", 0);
    multicastTxCacheSize = coCoviseConfig::getInt("COVER.MultiPC.Multicast.txCacheSize", 100000000);
    multicastTxCacheMin = coCoviseConfig::getInt("COVER.MultiPC.Multicast.txCacheMin", 1);
    multicastTxCacheMax = coCoviseConfig::getInt("COVER.MultiPC.Multicast.txCacheMax", 128);
    multicastTxRate = coCoviseConfig::getInt("COVER.MultiPC.Multicast.txRate", 1000);
    multicastBackoffFactor = (double)coCoviseConfig::getFloat("COVER.MultiPC.Multicast.backoffFactor", 0.0);
    multicastSockBuffer = coCoviseConfig::getInt("COVER.MultiPC.Multicast.sockBufferSize", 512000);
    multicastClientTimeout = coCoviseConfig::getInt("COVER.MultiPC.Multicast.readTimeoutSec", 30);
    multicastServerTimeout = coCoviseConfig::getInt("COVER.MultiPC.Multicast.writeTimeoutMsec", 500);
    multicastRetryTimeout = coCoviseConfig::getInt("COVER.MultiPC.Multicast.retryTimeout", 100);
    multicastMaxLength = coCoviseConfig::getInt("COVER.MultiPC.Multicast.maxLength", 1000000);

    string sm = coCoviseConfig::getEntry("COVER.MultiPC.SyncMode");
    numSlaves = coCoviseConfig::getInt("COVER.MultiPC.NumSlaves", 0);

    if (forceMpi)
    {
        syncMode = SYNC_MPI;
        MARK0("\tsyncMode: forced MPI");
        if (coVRConfig::instance()->debugLevel(3))
            fprintf(stderr, "syncMode: MPI\n");

#if !defined(HAS_MPI)
        fprintf(stderr, "This OpenCOVER does not have MPI support\n");
        exit(1);
#else
        MPI_Comm_size(appComm, &numSlaves);
        --numSlaves;
#endif
    }
    else if (strcasecmp(sm.c_str(), "TCP") == 0)
    {
        MARK0("\tsyncMode: TCP");
        if (coVRConfig::instance()->debugLevel(3))
            fprintf(stderr, "syncMode: TCP\n");

        syncMode = SYNC_TCP;
    }
    else if (strcasecmp(sm.c_str(), "SERIAL") == 0)
    {
        MARK0("\tsyncMode: SERIAL");
        if (coVRConfig::instance()->debugLevel(3))
            fprintf(stderr, "syncMode: SERIAL\n");

        syncMode = SYNC_SERIAL;
    }
    else if (strcasecmp(sm.c_str(), "MAGIC") == 0)
    {
        MARK0("\tsyncMode: MAGIC");
        if (coVRConfig::instance()->debugLevel(3))
            fprintf(stderr, "syncMode: MAGIC\n");

        syncMode = SYNC_MAGIC;
    }
    else if (strcasecmp(sm.c_str(), "TCP_SERIAL") == 0)
    {
        MARK0("\tsyncMode: TCP_SERIAL");
        if (coVRConfig::instance()->debugLevel(3))
            fprintf(stderr, "syncMode: TCP_SERIAL\n");

        syncMode = SYNC_TCP_SERIAL;
    }
    else if (strcasecmp(sm.c_str(), "PARALLEL") == 0)
    {
        MARK0("\tsyncMode: PARALLEL");
        if (coVRConfig::instance()->debugLevel(3))
            fprintf(stderr, "syncMode: PARALLEL\n");

        syncMode = SYNC_PARA;
    }
    else if (strcasecmp(sm.c_str(), "UDP") == 0)
    {
        MARK0("\tsyncMode: UDP");
        if (coVRConfig::instance()->debugLevel(3))
            fprintf(stderr, "syncMode: UDP\n");

        syncMode = SYNC_UDP;
    }
    else if (strcasecmp(sm.c_str(), "MULTICAST") == 0 && numSlaves > 0)
    {
        MARK0("\tsyncMode: MULTICAST");
        if (coVRConfig::instance()->debugLevel(3))
            fprintf(stderr, "syncMode: MULTICAST\n");

#if defined(NOMCAST) || !defined(HAVE_NORM)
        fprintf(stderr, "This OpenCOVER does not have MULTICAST support\n");
#else
        syncMode = SYNC_MULTICAST;
#endif
    }
    else if (strcasecmp(sm.c_str(), "MPI") == 0)
    {
        MARK0("\tsyncMode: MPI");
        if (coVRConfig::instance()->debugLevel(3))
            fprintf(stderr, "syncMode: MPI\n");

#if !defined(HAS_MPI)
        fprintf(stderr, "This OpenCOVER does not have MPI support\n");
#else
        syncMode = SYNC_MPI;
        MPI_Comm_size(appComm, &numSlaves);
        --numSlaves;
#endif
    }
    else
    {
        if (coVRConfig::instance()->debugLevel(3))
            fprintf(stderr, "syncMode: TCP\n");
        MARK0("\tsyncMode TCP");
    }

    syncProcess = SYNC_DRAW;
    sm = coCoviseConfig::getEntry("COVER.MultiPC.SyncProcess");
    if (strcasecmp(sm.c_str(), "APP") == 0)
    {
        if (coVRConfig::instance()->debugLevel(3))
            fprintf(stderr, "syncProcess: APP\n");
        syncProcess = SYNC_APP;

        MARK0("\tsyncProcess: APP");
    }
    else
    {
        MARK0("\tsyncProcess: DRAW");
        if (coVRConfig::instance()->debugLevel(3))
            fprintf(stderr, "syncProcess: DRAW\n");
    }

    if ((syncMode == SYNC_SERIAL) || (syncMode == SYNC_TCP_SERIAL))
    {
#ifndef _WIN32
        std::string deviceFile = coCoviseConfig::getEntry("value", "COVER.MultiPC.SerialDevice", "/dev/ttyd1");
        serial = open(deviceFile.c_str(), O_RDWR);
        if (serial == -1)
        {
            perror("ERROR: Could not open Serial port");
            cerr << "ERROR: deviceFile: " << deviceFile << endl;
            syncMode = SYNC_TCP;
            MARK1("\topening serial port %s: failed", deviceFile.c_str());
        }
        MARK1("\topening serial port %s: successful", deviceFile.c_str());

        fcntl(serial, F_SETFL, O_APPEND | O_NONBLOCK | FNDELAY);
        int statusByte;
        ioctl(serial, TIOCMGET, &statusByte);
        statusByte &= ~(TIOCM_RTS);
        if (ioctl(serial, TIOCMSET, &statusByte) == -1)
            cerr << "RTS=0 ERROR" << endl;
#endif
    }
    else if (syncMode == SYNC_MAGIC)
    {
#ifndef _WIN32
        // open 'wired' device and set to false
        std::string deviceFile = coCoviseConfig::getEntry("COVER.MultiPC.SerialDevice");
        magicFd = open(deviceFile.c_str(), O_RDWR);
        MARK1("\tMAGIC: opening port %s\n", deviceFile.c_str());

        // set my state to 'busy'
        char magicBuf = 0;
        if (write(magicFd, &magicBuf, 1) != 1)
        {
            cerr << "coVRMSController::coVRMSController: short write: " << strerror(errno) << endl;
        }
        MARK0("\tMAGIC: send BUSY");
#endif
    }
    else if (syncMode == SYNC_PARA)
    {
#ifdef __linux__
        std::string deviceFile = coCoviseConfig::getEntry("value", "COVER.MultiPC.ParallelDevice", "/dev/parport0");
        parallel = open(deviceFile.c_str(), O_RDWR);
        if (parallel == -1)
        {
            perror("ERROR: Could not open Parallel port");
            cerr << "ERROR: deviceFile: " << deviceFile << endl;
            syncMode = SYNC_TCP;
            MARK1("\tsyncMode: PARALLEL: port %s open failed", deviceFile.c_str());
        }
        MARK1("\tsyncMode: PARALLEL: port %s opened successful", deviceFile.c_str());

        ioctl(parallel, PPCLAIM);
        unsigned char statusByte = 0xff;
        statusByte = 0x0;
        ioctl(parallel, PPWDATA, &statusByte);
        allChildren = 0;
        for (int i = 0; i < numSlaves; i++)
            allChildren |= 1 << (i + 3);
#endif
    }

#ifdef HAS_MPI
    if (syncMode == SYNC_MPI)
    {
        MPI_Comm_rank(appComm, &myID);
        master = myID == 0;
        slave = !master;
    }
    else
#endif
    {
        /// This is a slave
        if (myID > 0)
        {
            master = false;
            slave = true;

            MARK1("COVER starting as slave %d", myID);
            if (coVRConfig::instance()->debugLevel(3))
                fprintf(stderr, "COVER starting as slave id=%d\n", myID);

#if !defined(NOMCAST) && defined(HAVE_NORM)
            if (syncMode == SYNC_MULTICAST)
            {
                if (!multicastAddress.empty())
                {
                    // Call client constructor with address/port
                    multicast = new Rel_Mcast(myID, multicastAddress.c_str(), multicastPort);
                }
                else
                {
                    // Call client constructor without address/port
                    multicast = new Rel_Mcast(myID);
                }

                if (!multicastInterface.empty())
                {
                    // Set an alternate interface (e.g. eth1)
                    multicast->setInterface(multicastInterface.c_str());
                }

                // Various settings from coconfig
                multicast->setDebugLevel(multicastDebugLevel);
                multicast->setBufferSpace(multicastBufferSpace);
                multicast->setSockBufferSize(multicastSockBuffer);
                multicast->setTimeout(multicastClientTimeout);
                multicast->setMaxLength(multicastMaxLength);

                if (multicast->init() != Rel_Mcast::RM_OK)
                {
                    delete multicast;
                    exit(0);
                }
            }
#endif

            connectToMaster(addr, port);
        }
        else
        {
            MARK0("COVER starting as master");
#if !defined(NOMCAST) && defined(HAVE_NORM)
            if (syncMode == SYNC_MULTICAST)
            {
                if (!multicastAddress.empty())
                {
                    multicast = new Rel_Mcast(true, numSlaves, multicastAddress.c_str(), multicastPort);
                }
                else
                {
                    multicast = new Rel_Mcast(true, numSlaves);
                }
                if (!multicastInterface.empty())
                {
                    // Set an alternate interface (e.g. eth1)
                    multicast->setInterface(multicastInterface.c_str());
                }

                // Various settings from coconfig
                multicast->setDebugLevel(multicastDebugLevel);
                multicast->setMTU(multicastMTU);
                multicast->setTTL(multicastTTL);
                multicast->setLoopback(multicastLoop);
                multicast->setBufferSpace(multicastBufferSpace);
                multicast->setBlocksAndParity(multicastBlockSize, multicastNumParity);
                multicast->setTxCacheBounds(multicastTxCacheSize, multicastTxCacheMin, multicastTxCacheMax);
                multicast->setTxRate(multicastTxRate);
                multicast->setBackoffFactor(multicastBackoffFactor);
                multicast->setSockBufferSize(multicastSockBuffer);
                multicast->setTimeout(multicastServerTimeout);
                multicast->setRetryTimeout(multicastRetryTimeout);
                multicast->setMaxLength(multicastMaxLength);

                if (multicast->init() != Rel_Mcast::RM_OK)
                {
                    delete multicast;
                    exit(0);
                }
            }
#endif
            if (coVRConfig::instance()->debugLevel(3))
                fprintf(stderr, "COVER starting as master\n");
        }
        stats[0] = NULL;
    }

    if (covise::coConfigConstants::getRank() != myID) {
        std::cerr << "coVRMSController: coConfigConstants::getRank()=" << covise::coConfigConstants::getRank() << ", myID=" << myID << std::endl;
    }
    assert(covise::coConfigConstants::getRank() == myID);
}

coVRMSController::~coVRMSController()
{
    delete socket;
    delete socketDraw;
    if ((syncMode == SYNC_SERIAL) || (syncMode == SYNC_TCP_SERIAL))
    {
        close(serial);
    }
    else if (syncMode == SYNC_PARA)
    {
#ifdef __linux__
        ioctl(parallel, PPRELEASE);
        close(parallel);
#endif
    }
    else if (syncMode == SYNC_MAGIC)
    {
        close(magicFd);
    }
    else if (syncMode == SYNC_MULTICAST)
    {
#if !defined(NOMCAST) && defined(HAVE_NORM)
        delete multicast;
#endif
    }
}

void
coVRMSController::killClients()
{
    if (syncMode == SYNC_MULTICAST)
    {
#if !defined(NOMCAST) && defined(HAVE_NORM)
        delete multicast;
#endif
    }
}

void coVRMSController::heartBeat(const std::string &name, bool draw)
{
    if (draw)
    {
        ++heartBeatCounterDraw;
    }
    else
    {
        ++heartBeatCounter;
    }
    int localCounter = draw ? heartBeatCounterDraw : heartBeatCounter;

    if (isMaster())
    {
        std::cerr << "coVRMSController: heart beat \"" << name << "\", count=" << localCounter << std::endl;
        if (draw)
            sendSlavesDraw(&localCounter, sizeof(localCounter));
        else
            sendSlaves(&localCounter, sizeof(localCounter));
    }
    else
    {
        int masterCount = 0;
        if (draw)
            readMasterDraw(&masterCount, sizeof(masterCount));
        else
            readMaster(&masterCount, sizeof(masterCount));
        if (localCounter != masterCount)
        {
            std::cerr << "coVRMSController: missed heart beat \"" << name << "\", master is " << masterCount << ", on " << getID() << " is " << localCounter << std::endl;
            exit(0);
        }
    }
}

void coVRMSController::checkMark(const char *file, int line)
{
    cerr << file << line << endl;
    std::stringstream str;
    str << file << ":" << line;
    heartBeat(str.str());
}

void
coVRMSController::statisticsCallback(void *, buttonSpecCell *spec)
{
    (void)spec;
    //   msController->drawStatistics=(bool)spec->state;
}

void coVRMSController::connectToMaster(const char *addr, int port)
{
    Host h(addr);
    socket = new Socket(&h, port, 200,10);

    int port2;
    readMaster(&port2, sizeof(port2), true);
    socketDraw = new Socket(&h, port2, 200, 0);
    int sendbuf = 64 * 1024;
    int recvbuf = 64 * 1024;
    if (setsockopt(socket->get_id(), SOL_SOCKET, SO_SNDBUF,
                   (char *)&sendbuf, sizeof(sendbuf)) < 0)
    {
        cerr << "could not set socket buff to " << sendbuf << endl;
    }
    if (setsockopt(socket->get_id(), SOL_SOCKET, SO_RCVBUF,
                   (char *)&recvbuf, sizeof(recvbuf)) < 0)
    {
        cerr << "could not set socket buff to " << sendbuf << endl;
    }
    if (setsockopt(socketDraw->get_id(), SOL_SOCKET, SO_SNDBUF,
                   (char *)&sendbuf, sizeof(sendbuf)) < 0)
    {
        cerr << "could not set socket buff to " << sendbuf << endl;
    }
    if (setsockopt(socketDraw->get_id(), SOL_SOCKET, SO_RCVBUF,
                   (char *)&recvbuf, sizeof(recvbuf)) < 0)
    {
        cerr << "could not set socket buff to " << sendbuf << endl;
    }
}

void coVRMSController::sendSlaves(const Message *msg)
{
    if (syncMode == SYNC_MULTICAST)
    {
#if !defined(NOMCAST) && defined(HAVE_NORM)

        // Prepare header
        int headerSize = 4 * sizeof(int);
        char header[headerSize];
        int *header_int;
        header_int = (int *)header;
        header_int[0] = msg->sender;
        header_int[1] = msg->send_type;
        header_int[2] = msg->type;
        header_int[3] = msg->length;

        // Write header via multicast
        if (multicast->write_mcast(header, headerSize) != Rel_Mcast::RM_OK)
        {
            delete multicast;
            exit(0);
        }

        // Write data via multicast (split up into pieces if necessary)
        int numMsg = msg->length / multicastMaxLength;
        if (msg->length % multicastMaxLength != 0)
            numMsg++;
        int curMsg;

        // Write first numMsg-1 messages
        for (curMsg = 1; curMsg < numMsg; curMsg++)
        {
            if (multicast->write_mcast(msg->data + multicastMaxLength * (curMsg - 1), multicastMaxLength) != Rel_Mcast::RM_OK)
            {
                delete multicast;
                exit(0);
            }
        }
        // Write numMsg message
        if (multicast->write_mcast(msg->data + multicastMaxLength * (curMsg - 1), msg->length % multicastMaxLength) != Rel_Mcast::RM_OK)
        {
            delete multicast;
            exit(0);
        }
#endif
    }
    else

    {
        int i;
        for (i = 0; i < numSlaves; i++)
        {
            slaves[i]->sendMessage(msg);
        }
#ifdef DEBUG_MESSAGES
        debugMessageCounter++;
#endif
    }
}

int coVRMSController::readMaster(Message *msg)
{
#if !defined(NOMCAST) && defined(HAVE_NORM)
    if (syncMode == SYNC_MULTICAST)
    {
        // Prepare header
        int headerSize = 4 * sizeof(int);
        char header[headerSize];
        int *header_int;

        // Receive header
        if (multicast->read_mcast(header, headerSize) != Rel_Mcast::RM_OK)
        {
            delete multicast;
            exit(0);
        }

        // Parse header, prepare data
        header_int = (int *)header;
        msg->sender = header_int[0];
        msg->send_type = (sender_type)header_int[1];
        msg->type = header_int[2];
        msg->length = header_int[3];
        msg->data = new char[msg->length];

        // Read data via multicast (Read piece-by-piece if necessary)
        int numMsg = msg->length / multicastMaxLength;
        if (msg->length % multicastMaxLength != 0)
            numMsg++;
        int curMsg;

        // Read first numMsg-1 messages
        for (curMsg = 1; curMsg < numMsg; curMsg++)
        {
            if (multicast->read_mcast(msg->data + multicastMaxLength * (curMsg - 1), multicastMaxLength) != Rel_Mcast::RM_OK)
            {
                delete multicast;
                exit(0);
            }
        }
        // Read numMsg message
        if (multicast->read_mcast(msg->data + multicastMaxLength * (curMsg - 1), msg->length % multicastMaxLength) != Rel_Mcast::RM_OK)
        {
            delete multicast;
            exit(0);
        }

        // Return size
        return msg->length + headerSize;
    }
    else
#endif
#ifdef HAS_MPI
        if (syncMode == SYNC_MPI)
    {
        const int headerSize = 4 * sizeof(int);

        int buffer[headerSize];

        int received = readMaster(buffer, headerSize);

        if (received < headerSize)
            return received;

        int *bufferInt = (int *)buffer;
        msg->sender = bufferInt[0];
        msg->send_type = bufferInt[1];
        msg->type = bufferInt[2];
        msg->length = bufferInt[3];

        msg->data = new char[msg->length];

        return received + readMaster(msg->data, msg->length);
    }
    else
#endif
    {
        char read_buf[4 * sizeof(int)];
        int *read_buf_int;
        int headerSize = 4 * sizeof(int);
        int toRead;
        int bytesRead = 0;
        int ret = readMaster(read_buf, headerSize);
        if (ret < headerSize)
            return ret;
        read_buf_int = (int *)read_buf;
        msg->sender = read_buf_int[0];
        msg->send_type = read_buf_int[1];
        msg->type = read_buf_int[2];
        msg->length = read_buf_int[3];
        msg->data = new char[msg->length];
#ifdef DEBUG_MESSAGES
        debugMessagesCheck = false;
#endif
        while (bytesRead < msg->length)
        {
            toRead = msg->length - bytesRead;
            if (toRead > READ_BUFFER_SIZE)
                toRead = READ_BUFFER_SIZE;
            int ret = readMaster(msg->data + bytesRead, toRead);
            if (ret < toRead)
            {
                //cerr << "Short Message" << ret << endl;
                if (ret < 0)
                    return ret;
            }
            bytesRead += ret;
        }
#ifdef DEBUG_MESSAGES
        debugMessagesCheck = true;
#endif
        return bytesRead;
    }
}

void coVRMSController::sendMaster(const Message *msg)
{
#ifdef HAS_MPI
    if (syncMode == SYNC_MPI)
    {
        int header[4];

        header[0] = msg->sender;
        header[1] = msg->send_type;
        header[2] = msg->type;
        header[3] = msg->length;

        sendMaster(reinterpret_cast<char *>(&header[0]), 4 * sizeof(int));
        sendMaster(msg->data, msg->length);
    }
    else
#endif
    {
        char write_buf[WRITE_BUFFER_SIZE];
        int *write_buf_int;
        int headerSize = 4 * sizeof(int);
        int len = msg->length + headerSize;
        int toWrite;
        int written = 0;
        toWrite = len;
        if (toWrite > WRITE_BUFFER_SIZE)
            toWrite = WRITE_BUFFER_SIZE;
        write_buf_int = (int *)write_buf;
        write_buf_int[0] = msg->sender;
        write_buf_int[1] = msg->send_type;
        write_buf_int[2] = msg->type;
        write_buf_int[3] = msg->length;
        if (toWrite > WRITE_BUFFER_SIZE)
            toWrite = WRITE_BUFFER_SIZE;
        memcpy(write_buf + headerSize, msg->data, toWrite - headerSize);
        sendMaster(write_buf, toWrite);
        written += toWrite;
        while (written < len)
        {
            toWrite = len - written;
            if (toWrite > WRITE_BUFFER_SIZE)
                toWrite = WRITE_BUFFER_SIZE;
            sendMaster(msg->data + written - headerSize, toWrite);
            written += toWrite;
        }
    }
}

// Default for readMaster: if multicast is set, do not send over TCP
int coVRMSController::readMaster(void *c, int n)
{
    return readMaster(c, n, false);
}
// bool mcastOverTCP: if using multicast, control whether to read via TCP socket
// Needs to be set true in calling function when:
//   SyncMode is Multicast AND (connecting OR syncing)
int coVRMSController::readMaster(void *c, int n, bool mcastOverTCP)
{
    int ret, read = 0;
    double startTime = 0.0;
#if defined(NOMCAST) || !defined(HAVE_NORM)
    (void)mcastOverTCP;
#endif
    if (drawStatistics)
    {
        startTime = cover->currentTime();
    }
#if !defined(NOMCAST) && defined(HAVE_NORM)
    if (syncMode == SYNC_MULTICAST && !mcastOverTCP)
    {
        if (multicast->read_mcast(c, n) != Rel_Mcast::RM_OK)
        {
            cerr << "multicast read failed" << endl;
            delete multicast;
            exit(0);
        }
        else
            return n;
    }
    else
#endif
#ifdef HAS_MPI
        if (syncMode == SYNC_MPI)
    {
#ifdef MPI_BCAST
        MPI_Bcast(const_cast<void *>(c), n, MPI_BYTE, 0, appComm);
#else
        MPI_Status status;
        MPI_Recv(c, n, MPI_BYTE, 0, AppTag, appComm, &status);
#endif

        if (drawStatistics)
        {
            networkRecv += cover->currentTime() - startTime;
        }

#ifdef MPI_BCAST
        return n;
#else
        int count;
        MPI_Get_count(&status, MPI_BYTE, &count);
        return count;
#endif
    }
    else
#endif
    {
#ifdef DEBUG_MESSAGES
        int checkRead = 0;
        int num;
        if (debugMessagesCheck)
        {
            while (checkRead < sizeof(n))
            {
                do
                {
                    ret = socket->Read((char *)(&(num)) + checkRead, sizeof(n) - checkRead);

                } while ((ret <= 0) && ((errno == EAGAIN) || (errno == EINTR)));
                if (ret < 0)
                    return ret;
                checkRead += ret;
                if (num != n)
                {
                    cerr << "tried to read " << n << " but received " << num << endl;
                    sleep(1000);
                }
            }
            checkRead = 0;
            while (checkRead < sizeof(debugMessageCounter))
            {
                do
                {
                    ret = socket->Read((char *)(&(num)) + checkRead, sizeof(debugMessageCounter) - checkRead);

                } while ((ret <= 0) && ((errno == EAGAIN) || (errno == EINTR)));
                if (ret < 0)
                    return ret;
                checkRead += ret;
                if (num != debugMessageCounter)
                {
                    cerr << "tried to read message number " << debugMessageCounter << " but received message number " << num << endl;
                    sleep(1000);
                }
            }
            debugMessageCounter++;
            sendMaster(&n, sizeof(n));
        }

#endif
        while (read < n)
        {
            do
            {
                ret = socket->Read((char *)c + read, n - read);

            } while ((ret <= 0) && ((errno == EAGAIN) || (errno == EINTR)));
            if (drawStatistics)
            {
                networkRecv += cover->currentTime() - startTime;
            }
            if (ret < 0)
                return ret;
            read += ret;
        }
    }
    return read;
}

void coVRMSController::sendMaster(const void *c, int n)
{
    int ret;
    double startTime = 0.0;
    if (drawStatistics)
    {
        startTime = cover->currentTime();
    }

#ifdef HAS_MPI
    if (syncMode == SYNC_MPI)
    {
        CO_MPI_SEND(const_cast<void *>(c), n, MPI_BYTE, 0, AppTag, appComm);
    }
    else
#endif
    {
        do
        {
            ret = socket->write(c, n);
            if (ret <= 0)
            {
                cerr << "Return Value = " << ret;
                perror("testit2:");
            }

        } while ((ret <= 0) && ((errno == EAGAIN) || (errno == EINTR)));
    }

    if (drawStatistics)
    {
        networkSend += cover->currentTime() - startTime;
    }
}

int coVRMSController::readSlave(int slaveNum, void *data, int num)
{
    return slaves[slaveNum]->read(data, num);
}

int coVRMSController::readSlaves(SlaveData *c)
{
    int i;
    int ret = 0;
    double startTime = 0.0;
    if (drawStatistics)
    {
        startTime = cover->currentTime();
    }
    for (i = 0; i < numSlaves; i++)
    {
        ret = slaves[i]->read(c->data.at(i), c->size());
        (void)ret;
        //       if(ret<c->size())
        //       {
        //          cerr << "coVRMSController::readSlaves err: slave " << i << ", error = " << ret << endl;
        //          perror("readSlaves error:");
        //          return -1;
        //       }
    }
    if (drawStatistics)
    {
        networkRecv += cover->currentTime() - startTime;
    }
    return c->size();
}

// Default for readMasterDraw: if multicast is set, do not send over TCP
int coVRMSController::readMasterDraw(void *c, int n)
{
    return readMasterDraw(c, n, false);
}
// bool mcastOverTCP: if using multicast, control whether to read via TCP socket
// Needs to be set true in calling function when:
//   SyncMode is Multicast AND (connecting OR syncing)
int coVRMSController::readMasterDraw(void *c, int n, bool mcastOverTCP)
{
#if defined(NOMCAST) || !defined(HAVE_NORM)
    (void)mcastOverTCP;
#else
    if (syncMode == SYNC_MULTICAST && !mcastOverTCP)
    {
        if (multicast->read_mcast(c, n) != Rel_Mcast::RM_OK)
        {
            cerr << "multicast read failed" << endl;
            delete multicast;
            exit(0);
        }
        else
            return n;
    }
    else
#endif
#ifdef HAS_MPI
    if (syncMode == SYNC_MPI)
    {
        MPI_Status status;
        MPI_Recv(const_cast<void *>(c), n, MPI_BYTE, 0, DrawTag, drawComm, &status);
        int count;
        MPI_Get_count(&status, MPI_BYTE, &count);
        return count;
    }
    else
#endif
    {
        int ret, read = 0;
        while (read < n)
        {
            do
            {
                ret = socketDraw->Read((char *)c + read, n - read);
            } while ((ret <= 0) && ((errno == EAGAIN) || (errno == EINTR)));
            if (ret < 0)
                return ret;
            read += ret;
        }
        return read;
    }

    return 0;
}

void coVRMSController::sendMasterDraw(const void *c, int n)
{
#ifdef HAS_MPI
    if (syncMode == SYNC_MPI)
    {
        CO_MPI_SEND(const_cast<void *>(c), n, MPI_BYTE, 0, DrawTag, drawComm);
    }
    else
#endif
    {
        int ret;
        do
        {
            ret = socketDraw->write(c, n);
            if (ret <= 0)
            {
                cerr << "sendMasterDraw: Return Value = " << ret;
                perror("testit2:");
            }

        } while ((ret <= 0) && ((errno == EAGAIN) || (errno == EINTR)));
    }
}

int coVRMSController::readSlavesDraw(void *c, int n)
{
    int i;
    int ret;
    for (i = 0; i < numSlaves; i++)
    {
        ret = slaves[i]->readDraw(c, n);
        if (ret < n)
        {
            if (ret == -1)
            {
                cerr << "coVRMSController::readSlavesDraw err: slave " << i << ", error = " << ret << endl;
                cerr << "Network error: " << errno << ", " << strerror(errno) << endl;
                perror("readSlavesDraw error:");
            }
            else
            {
                cerr << "coVRMSController::readSlavesDraw short read: slave " << i << ", expect=" << n << ", got=" << ret << endl;
                abort();
            }
            return -1;
        }
    }
    return n;
}

void coVRMSController::sendSlavesDraw(const void *c, int n)
{
#if !defined(NOMCAST) && defined(HAVE_NORM)
    if (syncMode == SYNC_MULTICAST)
    {
        if (multicast->write_mcast(c, n) != Rel_Mcast::RM_OK)
        {
            delete multicast;
            exit(0);
        }
    }
#endif
    int i;
    for (i = 0; i < numSlaves; i++)
    {
        slaves[i]->sendDraw(c, n);
    }
}

void coVRMSController::waitForSlavesDraw()
{
    char buf[100];
    if (master)
    {
        MARK0("COVER cluster master waiting for slaves to send sync");
        if (cover->debugLevel(5))
            fprintf(stderr, "COVER cluster master waiting for slaves to send sync");

        if (readSlavesDraw(buf, 1) < 0) // wait for all slaves
        {
            cerr << "sync_exit1 myID=" << myID << endl;
            exit(0);
        }
        MARK0("done");
    }
    else
    {
        MARK0("COVER cluster slave sending ID as sync");
        if (cover->debugLevel(5))
            fprintf(stderr, "COVER cluster slave sending ID as sync");
        *buf = (char)myID;
        sendMasterDraw(buf, 1);
    }
    MARK0("done");
    if (cover->debugLevel(5))
        fprintf(stderr, "done\n");
}

void coVRMSController::sendGoDraw()
{
    char buf[100];
    if (master)
    {
        MARK0("COVER cluster master send GO");
        *buf = 'g';
        //send go to all slaves
        sendSlavesDraw(buf, 1);
        MARK0("done");
    }
    else
    {
        MARK0("COVER cluster slave receive GO");

        if (readMasterDraw(buf, 1) < 1)
        {
            cerr << "sync_exit2 myID=" << myID << endl;
            exit(0);
        }
        MARK0("done");
    }
}

void coVRMSController::syncDraw()
{
    if (numSlaves == 0)
        return;
    MARK0("coVRMSController::syncDraw");
    if (cover->debugLevel(5))
        fprintf(stderr, "\ncoVRMSController::syncDraw\n");

    if (syncMode == SYNC_TCP)
    {
        waitForSlavesDraw();
        sendGoDraw();
    }
    else if (syncMode == SYNC_UDP)
    {
    }
    else if (syncMode == SYNC_TCP_SERIAL)
    {
        waitForSlavesDraw();
        if (master)
        {
            sendSerialGo();
        }
        else
        {
            waitForSerialGo();
        }
    }
    else if (syncMode == SYNC_SERIAL)
    {
        if (master)
        {
            waitForSerialGo();
            sendSerialGo();
        }
        else
        {
            sendSerialGo();
            waitForSerialGo();
        }
    }
    else if (syncMode == SYNC_MAGIC)
    {

        char magicBuf = 1;
        int wrbytes;
        // I am ready
        wrbytes = write(magicFd, &magicBuf, 1);
        if (wrbytes != 1)
        {
            cerr << "coVRMSController::sync: short write" << endl;
        }
        MARK0("\tMAGIC: send READY go WAITING");

        // wait till all are ready
        int status;
        wrbytes = read(magicFd, &magicBuf, 1);
        if (wrbytes != 1)
        {
            cerr << "coVRMSController::sync: short read" << endl;
        }
        status = magicBuf & 0x20;

        while (status == 0)
        {
            if (read(magicFd, &magicBuf, 1) != 1)
            {
                cerr << "coVRMSController::sync: short read2" << endl;
            }
            status = magicBuf & 0x20;
        }
        MARK0("\tMAGIC: received GO");
    }
    else if (syncMode == SYNC_PARA)
    {
        if (master)
        {
            waitForParallelJoin();
            sendParallelGo();
        }
        else
        {
            sendParallelGo();
            waitForParallelGo();
        }
    }
#if !defined(NOMCAST) && defined(HAVE_NORM)
    else if (syncMode == SYNC_MULTICAST)
    {
        waitForSlavesDraw(); // over TCP
        sendGoDraw(); // over Multicast
    }
#endif
#ifdef HAS_MPI
    else if (syncMode == SYNC_MPI)
    {
        waitForSlavesDraw();
        sendGoDraw();
    }
#endif
}

void coVRMSController::sendSlave(int i, const void *c, int n)
{
    double startTime = 0.0;
    if (drawStatistics)
    {
        startTime = cover->currentTime();
    }
#ifdef DEBUG_MESSAGES
    slaves[i]->send(&n, sizeof(n));
    slaves[i]->send(&debugMessageCounter, sizeof(debugMessageCounter));
    slaves[i]->read(&n, sizeof(n));
    debugMessageCounter++;
#endif
    slaves[i]->send(c, n);
    //std::cerr << i << " : " << (char*) data.data[i] << std::endl;
    //std::cerr << i << " : " << data.size() << std::endl;

    if (drawStatistics)
    {
        networkSend += cover->currentTime() - startTime;
    }
}

void coVRMSController::sendSlaves(const SlaveData &data)
{
    int i;
    double startTime = 0.0;
    if (drawStatistics)
    {
        startTime = cover->currentTime();
    }
#ifdef DEBUG_MESSAGES
    int n = data.size();
    for (i = 0; i < numSlaves; i++)
    {
        slaves[i]->send(&n, sizeof(n));
        slaves[i]->send(&debugMessageCounter, sizeof(debugMessageCounter));
        slaves[i]->read(&n, sizeof(n));
    }
    debugMessageCounter++;
#endif
    for (i = 0; i < numSlaves; i++)
    {
        slaves[i]->send(data.data[i], data.size());
        //std::cerr << i << " : " << (char*) data.data[i] << std::endl;
        //std::cerr << i << " : " << data.size() << std::endl;
    }

    if (drawStatistics)
    {
        networkSend += cover->currentTime() - startTime;
    }
}

void coVRMSController::sendSlaves(const void *c, int n)
{
    int i;
    double startTime = 0.0;
    if (drawStatistics)
    {
        startTime = cover->currentTime();
    }
#if !defined(NOMCAST) && defined(HAVE_NORM)
    if (syncMode == SYNC_MULTICAST)
    {
        if (multicast->write_mcast(c, n) != Rel_Mcast::RM_OK)
        {
            delete multicast;
            exit(0);
        }
    }
    else
#endif
#if defined HAS_MPI && defined MPI_BCAST
        if (syncMode == SYNC_MPI)
    {
        MPI_Bcast(const_cast<void *>(c), n, MPI_BYTE, AppTag, appComm);
        if (drawStatistics)
        {
            networkSend += cover->currentTime() - startTime;
        }
    }
    else
#endif
    {
#ifdef DEBUG_MESSAGES
        for (i = 0; i < numSlaves; i++)
        {
            slaves[i]->send(&n, sizeof(n));
            slaves[i]->send(&debugMessageCounter, sizeof(debugMessageCounter));

            slaves[i]->read(&n, sizeof(n));
        }
        debugMessageCounter++;
#endif
        for (i = 0; i < numSlaves; i++)
        {
            slaves[i]->send(c, n);
        }
    }
    if (drawStatistics)
    {
        networkSend += cover->currentTime() - startTime;
    }
}

void coVRMSController::startSlaves()
{
    if (numSlaves == 0)
    {
        return;
    }
    if (master)
    {
        int i;
        for (i = 0; i < numSlaves; i++)
        {
//cerr << "new coVRSlave(" << i+1 << ")" << endl;
#ifdef HAS_MPI
            if (syncMode == SYNC_MPI)
            {
                slaves[i] = new coVRMpiSlave(i + 1, appComm, drawComm);
            }
            else
#endif
            {
                slaves[i] = new coVRTcpSlave(i + 1);
            }
        }
        for (i = 0; i < numSlaves; i++)
        {
            //cerr << "slaves[" << i << "]->start(" << endl;
            slaves[i]->start();
        }
        for (i = 0; i < numSlaves; i++)
        {
            slaves[i]->accept();
        }
#ifdef DEBUG_MESSAGES
        debugMessageCounter++;
#endif
    }
}

void coVRMSController::waitForSlaves()
{
    char buf[100];
    if (master)
    {

        static SlaveData result(1);
        MARK0("COVER cluster master waiting for slaves to send sync");
        if (cover->debugLevel(5))
            fprintf(stderr, "COVER cluster master waiting for slaves to send sync");

        if (readSlaves(&result) < 0) // wait for all slaves
        {
            cerr << "sync_exit1 myID=" << myID << endl;
            exit(0);
        }
        MARK0("done");
    }
    else
    {
        MARK0("COVER cluster slave sending ID as sync");
        if (cover->debugLevel(5))
            fprintf(stderr, "COVER cluster slave sending ID as sync");
        *buf = (char)myID;
        sendMaster(buf, 1);
    }
    MARK0("done");
    if (cover->debugLevel(5))
        fprintf(stderr, "done\n");
}

void coVRMSController::waitForMaster()
{
    char buf[100];
    if (master)
    {
        MARK0("COVER cluster master waiting for master to send sync");
        if (cover->debugLevel(5))
            cerr << "COVER cluster master waiting for master to send sync" << endl;
        sendSlaves(buf, 1);
        MARK0("done");
    }
    else
    {
        MARK0("COVER cluster slave sending ID as sync");
        if (cover->debugLevel(5))
            cerr << "COVER cluster slave sending ID as sync" << endl;
        *buf = (char)myID;
        readMaster(buf, 1);
    }
    MARK0("done");
    if (cover->debugLevel(5))
        cerr << "waitforslaves done" << endl;
}

void coVRMSController::sendGo()
{
    char buf[100];
    if (master)
    {
        MARK0("COVER cluster master send GO");
        *buf = 'g';
        //send go to all slaves
        sendSlaves(buf, 1);
        MARK0("done");
    }
    else
    {
        MARK0("COVER cluster slave receive GO");

        if (readMaster(buf, 1) < 1)
        {
            cerr << "sync_exit2 myID=" << myID << endl;
            exit(0);
        }
        MARK0("done");
    }
}

void coVRMSController::startupSync()
{
    if (cover->debugLevel(3))
        fprintf(stderr, "\ncoVRMSController::startupSync\n");

    if (numSlaves == 0)
        return;
    if ((syncMode == SYNC_SERIAL) || (syncMode == SYNC_MAGIC) || (syncMode == SYNC_PARA))
    {
        waitForSlaves();
        sendGo();
    }
    else
    {
        sync();
    }
}

void coVRMSController::sync()
{
    if (numSlaves == 0)
        return;
    MARK0("coVRMSController::sync");
    if (cover->debugLevel(5))
        fprintf(stderr, "\ncoVRMSController::sync\n");

    if (syncMode == SYNC_TCP)
    {
        waitForSlaves();
        sendGo();
    }
    else if (syncMode == SYNC_UDP)
    {
    }
    else if (syncMode == SYNC_TCP_SERIAL)
    {
        waitForSlaves();
        if (master)
        {
            sendSerialGo();
        }
        else
        {
            waitForSerialGo();
        }
    }
    else if (syncMode == SYNC_SERIAL)
    {
        if (master)
        {
            waitForSerialGo();
            sendSerialGo();
        }
        else
        {
            sendSerialGo();
            waitForSerialGo();
        }
    }
    else if (syncMode == SYNC_MAGIC)
    {

        char magicBuf = 1;
        int wrbytes;
        // I am ready
        wrbytes = write(magicFd, &magicBuf, 1);
        if (wrbytes != 1)
        {
            cerr << "coVRMSController::sync: short write" << endl;
        }
        MARK0("\tMAGIC: send READY go WAITING");

        // wait till all are ready
        int status;
        wrbytes = read(magicFd, &magicBuf, 1);
        if (wrbytes != 1)
        {
            cerr << "coVRMSController::sync: short read" << endl;
        }
        status = magicBuf & 0x20;

        while (status == 0)
        {
            if (read(magicFd, &magicBuf, 1) != 1)
            {
                cerr << "coVRMSController::sync: short read2" << endl;
            }
            status = magicBuf & 0x20;
        }
        MARK0("\tMAGIC: received GO");
    }
    else if (syncMode == SYNC_PARA)
    {
        if (master)
        {
            waitForParallelJoin();
            sendParallelGo();
        }
        else
        {
            sendParallelGo();
            waitForParallelGo();
        }
    }
#if !defined(NOMCAST) && defined(HAVE_NORM)
    else if (syncMode == SYNC_MULTICAST)
    {
        waitForSlaves(); // over TCP
        sendGo(); // over Multicast
    }
#endif
#ifdef HAS_MPI
    else if (syncMode == SYNC_MPI)
    {
        waitForSlaves();
        sendGo();
    }
#endif
}

void coVRMSController::sendSerialGo()
{
    MARK0("coVRMSController::sendSerialGo");
#ifndef _WIN32
    int statusByte;
    static bool state = true;
    if (state)
    {
        ioctl(serial, TIOCMGET, &statusByte);
        statusByte |= TIOCM_RTS;
        if (ioctl(serial, TIOCMSET, &statusByte) == -1)
            cerr << "RTS=1 ERROR" << endl;
        state = false;
    }
    else
    {
        ioctl(serial, TIOCMGET, &statusByte);
        statusByte &= ~(TIOCM_RTS);
        if (ioctl(serial, TIOCMSET, &statusByte) == -1)
            cerr << "RTS=0 ERROR" << endl;
        state = true;
    }
#endif
}

void coVRMSController::waitForSerialGo()
{
    MARK0("coVRMSController::waitForSerialGo");
#ifndef _WIN32
    int statusByte = 0;
    static bool state = false;
    do
    {
        ioctl(serial, TIOCMGET, &statusByte);
    } while ((statusByte & TIOCM_CTS) == state);
    state = !state;
#endif
}

void coVRMSController::sendParallelGo()
{
#ifdef __linux__
    static bool state = false;
    unsigned char statusByte = 0xff;
    if (state)
        statusByte = 0x0;
    ioctl(parallel, PPWDATA, &statusByte);
    /*if(master)
   fprintf(stderr,"Master");
   fprintf(stderr,"Go\n");*/
    state = !state;
#endif
}

void coVRMSController::waitForParallelGo()
{
    int myBit;
    myBit = 1 << (myID + 2);
    myBit = 1 << 3;
/* int i;
       for(i=0;i<8;i++)
       {
        if(myBit & (1<<i))
        {
           fprintf(stderr,"1");
      }
       else
       {
          fprintf(stderr,"0");
          }
   }
   fprintf(stderr,"myBit\n");*/

#ifdef __linux__
    static bool state = false;
    //fprintf(stderr,"myID: %d s=%d\n",myID,state);
    /*if(master)
   fprintf(stderr,"Master");
   fprintf(stderr,"wait\n");*/
    unsigned char statusByte = 0x0;
    if (state)
    {
        do
        {
            ioctl(parallel, PPRSTATUS, &statusByte);
            /*	 int i;
                for(i=0;i<8;i++)
                {
                 if(statusByte & (1<<i))
                 {
                    fprintf(stderr,"1");
               }
                else
                {
                   fprintf(stderr,"0");
                   }
         }
         fprintf(stderr,"wait\n");*/

        } while (statusByte & myBit);
    }
    else
    {
        do
        {
            ioctl(parallel, PPRSTATUS, &statusByte);
            /*int i;
            for(i=0;i<8;i++)
            {
             if(statusByte & (1<<i))
             {
                fprintf(stderr,"1");
           }
            else
            {
               fprintf(stderr,"0");
               }
         }
         fprintf(stderr,"!wait\n");
         for(i=0;i<8;i++)
         {
         if(myBit & (1<<i))
         {
         fprintf(stderr,"1");
         }
         else
         {
         fprintf(stderr,"0");
         }
         }
         fprintf(stderr,"myBit\n");*/

        } while (!(statusByte & myBit));
    }
    //fprintf(stderr,"finished myID: %d s=%d\n",myID,state);
    /*if(master)
   fprintf(stderr,"Master");
   fprintf(stderr,"waitFinished\n");*/
    state = !state;
#endif
}

void coVRMSController::waitForParallelJoin()
{
#ifdef __linux__
    /*int i;
      for(i=0;i<8;i++)
      {
       if(allChildren & (1<<i))
       {
          fprintf(stderr,"1");
          }
           else
      {
         fprintf(stderr,"0");
         }
   }
   fprintf(stderr,"AllChildren\n");

   if(master)
   fprintf(stderr,"Master");
   fprintf(stderr,"join\n"); */

    static bool state = true;
    //fprintf(stderr,"join myID: %d s=%d\n",myID,state);
    unsigned char statusByte = 0x0;
    if (state)
    {
        do
        {
            ioctl(parallel, PPRSTATUS, &statusByte);
            /*	 int i;
                for(i=0;i<8;i++)
                {
                 if(statusByte & (1<<i))
                 {
                    fprintf(stderr,"1");
               }
                else
                {
                   fprintf(stderr,"0");
                   }
         }
         fprintf(stderr,"join\n");
         for(i=0;i<8;i++)
         {
         if(allChildren & (1<<i))
         {
         fprintf(stderr,"1");
         }
         else
         {
         fprintf(stderr,"0");
         }
         }
         fprintf(stderr,"AllChildren\n");
         fprintf(stderr,"s&a%d\n",(statusByte&allChildren));*/

        } while (!((statusByte & allChildren) == allChildren));
    }
    else
    {
        do
        {
            /*         ioctl(parallel, PPRSTATUS, &statusByte);
             int i;
                for(i=0;i<8;i++)
                {
                 if(statusByte & (1<<i))
                 {
                    fprintf(stderr,"1");
               }
                else
                {
                   fprintf(stderr,"0");
         }
         }
         fprintf(stderr,"!join\n");*/

        } while (!((statusByte & allChildren) == 0));
    }
    //fprintf(stderr,"join finished myID: %d s=%d\n",myID,state);
    state = !state;
/* if(master)
    fprintf(stderr,"Master");
    fprintf(stderr,"joinFinished\n");*/
#endif
}

void coVRMSController::syncApp(int frameNum)
{
    if (numSlaves == 0)
        return;
    if (master)
    {
        sendSlaves(&frameNum, sizeof(frameNum));
    }
    else
    {
        int masterFrameNum = 0;
        if (readMaster(&masterFrameNum, sizeof(masterFrameNum)) < 0)
        {
            cerr << "bcould not read message from Master" << endl;
            cerr << "sync_exit15a myID=" << myID << endl;
            exit(0);
        }
        if (masterFrameNum != frameNum)
        {
            cerr << "frame numbers differ" << endl;
            cerr << "myID=" << myID << endl;
            exit(0);
        }
    }
    if (syncProcess != SYNC_APP)
        return;

    //double sTime=0.0;
    //sTime = cover->currentTime();
    //cerr << "id: " << myID << " time: " << cover->currentTime()-sTime << endl;

    MARK0("COVER syncApp");
    if (cover->debugLevel(5))
        fprintf(stderr, "\ncoVRMSController::syncApp\n");

    sync();
    MARK0("COVER syncApp done");
}

//sync Time and handle Cluster statistics

void coVRMSController::syncTime()
{

    if (numSlaves == 0)
        return;
    int i;
    static bool oldStat = false;
    if ((oldStat != drawStatistics) && (master) && cover->getScene() != 0)
    {
        if (stats[0] == NULL)
        {
            for (i = 0; i < numSlaves + 1; i++)
            {
                stats[i] = new coClusterStat(i);
            }
        }
        if (drawStatistics)
        {
            for (i = 0; i < numSlaves + 1; i++)
            {
                stats[i]->show();
            }
        }
        else
        {
            for (i = 0; i < numSlaves + 1; i++)
            {
                stats[i]->hide();
            }
        }
        oldStat = drawStatistics;
    }
    if (drawStatistics && cover->getScene() != 0)
    {
        static double lastTime = 0;
        double currentTime = cover->currentTime();
        if (master)
        {
            unsigned int ret;
            char buf[3 * sizeof(double)];
            stats[numSlaves]->updateMinMax(currentTime - lastTime, networkSend, networkRecv);
            for (i = 0; i < numSlaves; i++)
            {
                ret = slaves[i]->read(buf, 3 * sizeof(double));
                if (ret < 3 * sizeof(double))
                {
                    cerr << "Return Value = " << ret << "slave" << i << endl;
                    perror("readSlaves error:");
                    return;
                }

                double frameTime;
                memcpy(&frameTime, buf, sizeof(double));
                memcpy(&networkSend, buf + sizeof(double), sizeof(double));
                memcpy(&networkRecv, buf + 2 * sizeof(double), sizeof(double));
                stats[i]->updateMinMax(frameTime, networkSend, networkRecv);
                fprintf(stderr, "slave: % 2d frameTime: %-10.5lf networkRecv: %-10.5lf networkSend: %-10.5lf\n", i, frameTime, networkRecv, networkSend);
                //cerr << "slave: " << i << " frameTime: " << frameTime << " networkRecv: "<< networkRecv << " networkSend: "<< networkSend<< endl;
            }
            //get global min/max to be able to compare graphs
            float globalMax = 0;
            float globalSendMax = 0;
            float globalRecvMax = 0;
            for (i = 0; i < numSlaves; i++)
            {
                if (stats[i]->renderMax > globalMax)
                {
                    globalMax = stats[i]->renderMax;
                }
                if (stats[i]->sendMax > globalSendMax)
                {
                    globalSendMax = stats[i]->sendMax;
                }
                if (stats[i]->recvMax > globalRecvMax)
                {
                    globalRecvMax = stats[i]->recvMax;
                }
            }
            stats[numSlaves]->updateValues();
            for (i = 0; i < numSlaves; i++)
            {
                stats[i]->renderMax = globalMax;
                stats[i]->sendMax = globalSendMax;
                stats[i]->recvMax = globalRecvMax;
                stats[i]->updateValues();
            }
        }
        else
        {
            char buf[3 * sizeof(double)];
            // sendTime Diff to master
            int len = 0;
            *((double *)(buf + len)) = currentTime - lastTime;
            len += sizeof(double);
            *((double *)(buf + len)) = networkSend;
            len += sizeof(double);
            *((double *)(buf + len)) = networkRecv;
            len += sizeof(double);
            sendMaster(buf, len);
        }
        networkRecv = 0;
        networkSend = 0;
        lastTime = currentTime;
    }

    if (cover->debugLevel(4))
        fprintf(stderr, "\ncoVRMSController::syncTime\n");

    double frameTime, frameRealTime;
    if (master)
    {
        frameTime = cover->frameTime();
        frameRealTime = cover->frameRealTime();
        sendSlaves(&frameTime, sizeof(double));
        sendSlaves(&frameRealTime, sizeof(double));
    }
    else
    {
        if (readMaster(&frameTime, sizeof(double)) < 0
            || readMaster(&frameRealTime, sizeof(double)) < 0)
        {
            cerr << "ccould not read message from Master" << endl;
            cerr << "sync_exit14 myID=" << myID << endl;
            exit(0);
        }
        cover->setFrameTime(frameTime);
        cover->setFrameRealTime(frameRealTime);
    }

    if (syncMode == SYNC_MAGIC)
    {
        waitForSlaves();
        waitForMaster();
        // I am busy again
        char magicBuf = 0;
        if (write(magicFd, &magicBuf, 1) != 1)
        {
            cerr << "coVRMSController::syncTime: short write" << endl;
        }
        MARK0("\tMAGIC: send BUSY (after tcp sync with acknowledge\n");
    }
}

int coVRMSController::syncData(void *data, int size)
{
    if (isMaster())
    {
        sendSlaves(data, size);
    }
    else
    {
        if (readMaster(data, size) < 0)
        {
            cerr << "dcould not read message from Master" << endl;
            cerr << "sync_exit15b myID=" << myID << endl;
            exit(0);
        }
    }
    return size;
}

bool coVRMSController::syncBool(bool state)
{
    if (numSlaves == 0)
        return state;

    char c = state;
    if (master)
    {
        sendSlaves(&c, 1);
    }
    else
    {

        if (readMaster(&c, 1) < 0)
        {
            cerr << "ccould not read message from Master" << endl;
            cerr << "sync_exit15c myID=" << myID << endl;
            exit(0);
        }
        state = c != '\0';
    }
    return state;
}

void coVRMSController::syncVRBMessages()
{
#define MAX_VRB_MESSAGES 500
    Message *vrbMsgs[MAX_VRB_MESSAGES];
    int numVrbMessages = 0;
    if (numSlaves == 0)
        return;

    if (cover->debugLevel(4))
        fprintf(stderr, "\ncoVRMSController::syncVRBMessages\n");

    Message *vrbMsg = new Message;
    if (master)
    {
        if (vrbc && vrbc->isConnected())
        {
            while (vrbc->poll(vrbMsg))
            {
                vrbMsgs[numVrbMessages] = vrbMsg;
                numVrbMessages++;
                vrbMsg = new Message;
                if (numVrbMessages >= MAX_VRB_MESSAGES)
                {
                    cerr << "to many VRB Messages!!" << endl;
                    break;
                }
                if (!vrbc->isConnected())
                    break;
            }
        }
        else
        {
            static double oldSec = 0;
            double curSec;
            curSec = cover->frameTime();

            // try to reconnect
            if ((curSec - oldSec) > 2.0)
            {
                if (cover->debugLevel(3))
                {
                    fprintf(stderr, "trying to establish VRB connection\n");
                }

                if (vrbc == NULL)
                    vrbc = new VRBClient("COVER", coVRConfig::instance()->collaborativeOptionsFile);
                vrbc->connectToServer();
                oldSec = curSec;
            }
        }
        sendSlaves(&numVrbMessages, sizeof(int));
        //cerr << "numMasterMSGS " <<  numVrbMessages << endl;
        int i;
        for (i = 0; i < numVrbMessages; i++)
        {
            sendSlaves(vrbMsgs[i]);
            coVRCommunication::instance()->handleVRB(vrbMsgs[i]);
            vrbMsgs[i]->data = NULL;
            delete vrbMsgs[i];
        }
    }
    else
    {
        //get number of Messages
        if (readMaster(&numVrbMessages, sizeof(int)) < 0)
        {
            cerr << "sync_exit16 myID=" << myID << endl;
            exit(0);
        }
        //cerr << "numSlaveMSGS " <<  numVrbMessages << endl;
        int i;
        for (i = 0; i < numVrbMessages; i++)
        {
            if (readMaster(vrbMsg) < 0)
            {
                cerr << "sync_exit17 myID=" << myID << endl;
                exit(0);
            }
            coVRCommunication::instance()->handleVRB(vrbMsg);
        }
    }
    vrbMsg->data = NULL;
    delete vrbMsg;
}

void coVRMSController::loadFile(const char *filename)
{
    if (filename)
    {
        char buf[1000];
        snprintf(buf, 1000, "loading %s", filename);
        OpenCOVER::instance()->hud->setText3(buf);
    }
    if (numSlaves == 0)
    {
        if (filename != NULL)
        {
            coVRFileManager::instance()->loadFile(filename);
        }
        return;
    }

    if (cover->debugLevel(3))
        fprintf(stderr, "\ncoVRMSController::loadFile\n");
    int len = 0;
    if (master)
    {
        if (filename)
            len = strlen(filename) + 1;
        sendSlaves(&len, sizeof(int));
        if (len > 0)
            sendSlaves(filename, len);

        if (filename != NULL)
        {
            coVRFileManager::instance()->loadFile(filename);
        }
    }
    else
    {
        int numcs;
        if (readMaster(&numcs, sizeof(int)) < 0)
        {
            cerr << "bcould not read message from Master" << endl;
        }
        cerr << "numcs" << numcs << endl;
        if (numcs)
        {
            char *buf = new char[numcs];
            if (readMaster(buf, numcs) < 0)
            {
                cerr << "ccould not read message from Master" << endl;
            }
            coVRFileManager::instance()->loadFile(buf);
            delete[] buf;
        }
    }
}
