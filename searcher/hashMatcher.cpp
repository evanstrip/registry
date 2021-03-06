/*******************************************************************************
 * hashMatcher.cpp
 * 
 * A simple server that stores a giant lot of 64bit hashes in memory (tested 
 * with 100 million) and lets you find all the entries in that list that have 
 * a hamming distance less than X compared with the query hash.
 * 
 * Author: Andrew Smith
 * Licence: AGPL v3
 */

#include <list>
#include <vector>
#include <forward_list>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <cstdio>
#include <time.h>
#include <stdlib.h>
#include <pthread.h>
#include <inttypes.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "hashMatcher.h"

#define SOCKET_PATH "/tmp/searcher.sock"
#define CONNECTION_QUEUE_SIZE 10

/**
 * These hold all the data to compare against. They take up a lot of memory
 * so don't add stuff here willy-nilly.
 */
struct Node
{
    uint64_t dbId;
    uint64_t pHash;
    
    Node(uint64_t dbId, uint64_t pHash)
    {
        this->dbId = dbId;
        this->pHash = pHash;
    }
};

/**
 * These are used to record a result of a search.
 */
struct Match
{
    uint64_t dbId;
    int distance;
    
    Match(uint64_t dbId, int distance)
    {
        this->dbId = dbId;
        this->distance = distance;
    }
};

/**
 * Parameters to pass to a search thread
 */
struct ThreadParam
{
    unsigned threadNum;         // Just for debugging
    uint64_t queryHash;         // My "search string"
    int maxDistance;            // Threshold for results
    std::list<Node>* nodeList;  // The list to search in this thread
};

// This is set as a command-line parameter and is used to configure the
// number of search threads (and associated things).
unsigned GBLnumCores;
// Array of lists of nodes to search. One list per thread for efficiency.
std::list<Node>* GBLnodeLists;
// Array of threads to do the searching of the lists above.
pthread_t* GBLsearchThreads;
// Search results
std::vector<Match> GBLsearchResults;
// Mutex for the vector above since it's updated from multiple threads
pthread_mutex_t GBLsearchResultsMutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * Add a node to the shortest list (or at least to one that's not the longest)
 */
void add(uint64_t hash, uint64_t dbId)
{
    // Look through all the lists. If the second is shorter than the first: add
    // to that. Else compare second and third, etc. 
    // If all lists are the same length or (very unlikely) longer lists follow
    // shorter ones: add to the first list.
    bool added = false;
    for (unsigned i = 0; i < GBLnumCores - 1; i++)
    {
        if (GBLnodeLists[i+1].size() < GBLnodeLists[i].size())
        {
            GBLnodeLists[i+1].emplace_front(hash, dbId);
            added = true;
        }
    }
    if (!added)
        GBLnodeLists[0].emplace_front(hash, dbId);
}

/**
 * Complain about bad parameters and exit.
 */
void printUsageAndExit()
{
    printf("Bad parameters. Usage:\n\n"
           "searcher -c NUM_CORES\n\n"
           "Then connect to the socket %s and send a 'match' or 'add' command\n"
           "(one command per connection)\n\n"
           "match hash_uint64_in_hex max_distance_uint8_in_decimal\n"
           "add dbId_uint64_in_decimal hash_uint64_in_hex\n", SOCKET_PATH);
    exit(1);
}

/**
 * Run command from client. Valid commands:
 * 
 * "match hash_uint64_in_hex max_distance_uint8_in_decimal\n" 
 * (max length of hash is 0xFFFFFFFFFFFFFFFF, 
 *  max_distance is a byte, most likely single, maybe double-digits 
 *  that's a total max of 29 characters including the newline)
 * Returns newline-separated list of pairs of dbId and distance:
 * "uint64_in_decimal uint8_in_decimal\n"
 * 
 * "add dbId_uint64_in_decimal hash_uint64_in_hex\n"
 * (max total length 44 characters including the newline)
 * Returns one of "Inserted OK\n" or "Failed to insert: reason\n"
 */
void processCommand(int socket, const char* command)
{
    int rc;
    uint64_t hash;
    unsigned char maxDistance;
    uint64_t dbId;
    
    // See if this is a match command
    rc = sscanf(command, "match %" SCNx64 " %hhu\n", &hash, &maxDistance);
    if (rc == 2)
    {
        printf("Will search for %" PRIx64 " (max distance %u)\n", 
               hash, maxDistance);
        search(hash, maxDistance);
        
        // The results (if any) are all in GBLsearchResults. Send them back
        // to the client.
        for (unsigned i = 0; i < GBLsearchResults.size(); i++)
        {
            char response[100];
            unsigned responseLen, numBytesSent;
            responseLen = sprintf(response, "%" PRIu64 " %u\n", 
                    GBLsearchResults[i].dbId, GBLsearchResults[i].distance);
            
            numBytesSent = send(acceptedSocket, response, responseLen, 
                                MSG_NOSIGNAL);
            if (numBytesSent != responseLen)
            {
                fprintf(stderr, "Couldn't send response to client\n");
                break;
            }
        }
    }
    else // See if this is an add command
    {
        rc = sscanf(command, "add %" SCNu64 " %" SCNx64 "\n", &dbId, &hash);
        if (rc == 2)
        {
            printf("Will add %lu 0x%lX\n", dbId, hash);
            add(dbId, hash);
        }
        else
            fprintf(stderr, "Invalid command received\n");
    }
}

/**
 * Keep reading newline-separated commands from the socket. Then pass each 
 * command to processCommand().
 */
void readCommands(int socket)
{
    const int bufferSize = 1024;
    char buffer[bufferSize + 1]; // more than enough for one command + '\0'
    int numBytesFilled = 0;
    int rc;
    
    while (true)
    {
        rc = recv(socket, buffer + numBytesFilled, 
                  bufferSize - numBytesFilled, 0);
        if (rc == 0)
            break; // connection closed
        if (rc == -1)
        {
            perror("Error reading command from client: ");
            break;
        }
        numBytesFilled += rc;
        
        char* lineStart = buffer;
        char* lineEnd;
        while ( (lineEnd = (char*)memchr((void*)lineStart, '\n', 
                                        numBytesFilled - (lineStart - buffer))))
        {
            *lineEnd = '\0';
            processCommand(socket, lineStart);
            lineStart = lineEnd + 1;
        }
        
        /* Shift buffer down so the unprocessed data is at the start */
        numBytesFilled -= (lineStart - buffer);
        memmove(buffer, lineStart, numBytesFilled);
        
        if (numBytesFilled == bufferSize)
        {
            fprintf(stderr, "Command too long, closing connection.\n");
            break;
        }
    }
}

/**
 * Start threads to do the search, and wait for them to finish. Results are in
 * the global GBLsearchResults
 */
void search(uint64_t hash, unsigned char maxDistance)
{
    GBLsearchResults.clear();
    
    ThreadParam* threadParam = new ThreadParam[GBLnumCores];
    
    // Start the search on all threads.
    for (unsigned i = 0; i < GBLnumCores; i++)
    {
        threadParam[i].threadNum = i;
        threadParam[i].queryHash = hash;
        threadParam[i].maxDistance = maxDistance;
        threadParam[i].nodeList = &(GBLnodeLists[i]);
        int rc = pthread_create(&GBLsearchThreads[i], NULL, 
                                searchThread, 
                                (void *)(&threadParam[i]));
        if (rc != 0)
        {
            printf("Error: pthread_create() returned %d\n", rc);
            exit(2);
        }
    }
    
    // Wait for all the threads to finish. That should happen at about
    // the same time because the lists are the same size and the operations
    // almost always the same length.
    for (unsigned i = 0; i < GBLnumCores; i++)
        pthread_join(GBLsearchThreads[i], NULL);
}

/**
 * Go through a list of Nodes and for each one - do a hamming distance
 * calculation. Put results into the global GBLsearchResults.
 */
void* searchThread(void* threadParam)
{
    //unsigned threadNum = ;
    uint64_t queryHash = ((ThreadParam*)threadParam)->queryHash;
    int maxDistance = ((ThreadParam*)threadParam)->maxDistance;
    std::list<Node>* nodeList = ((ThreadParam*)threadParam)->nodeList;
    
    //printf("Thread number %d is now working\n", 
             //((ThreadParam*)threadParam)->threadNum);fflush(NULL);
    
    // BEGIN PERFORMANCE-CRITICAL SECTION
    for (std::list<Node>::iterator it = nodeList->begin(); 
         it != nodeList->end(); it++)
    {
        // The next two lines are the ones that need to be optimised
        uint64_t bitsToCount = queryHash ^ it->pHash;
        int distance = __builtin_popcountl(bitsToCount);
        
        if (distance <= maxDistance)
        {
            //printf("Ditance %d between 0x%lX and 0x%lX (bits 0x%lX)\n",
                   //distance, queryHash, it->pHash, bitsToCount);
            
            pthread_mutex_lock(&GBLsearchResultsMutex);
            GBLsearchResults.emplace_back(it->dbId, distance);
            //!! Set a limit on the number of results and check it here (so I don't run out of memory, etc)
            pthread_mutex_unlock(&GBLsearchResultsMutex);
        }
    }
    // END PERFORMANCE-CRITICAL SECTION
    
    //printf("Thread number %d is done\n", 
            //((ThreadParam*)threadParam)->threadNum);fflush(NULL);
    
    pthread_exit(NULL);
}

int main(int argc, char** argv)
{
    int c;
    int rc;
    
    // Parse arguments to figure out how many threads to have
    bool paramsOk = false;
    while ((c = getopt (argc, argv, "c:")) != -1)
    {
        if (c == 'c')
        {
            int rc;
            rc = sscanf(optarg, "%d", &GBLnumCores);
            if (rc != 1 || GBLnumCores < 1)
                printUsageAndExit();
            else
                paramsOk = true;
        }
        else if (c == '?')
            printUsageAndExit();
    }
    if (!paramsOk)
        printUsageAndExit();
    
    // Allocate the array of lists, one list per core
    GBLnodeLists = new std::list<Node>[GBLnumCores];
    
    // Allocate the array of threads for searching, one thread per core
    GBLsearchThreads = new pthread_t[GBLnumCores];
    
    // BEGIN set up listening socket
    int listeningSocket;
    int len;
    struct sockaddr_un listeningAddr;
    
    listeningSocket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listeningSocket == -1)
    {
        perror("Couldn't create server socket: ");
        exit(3);
    }
    
    listeningAddr.sun_family = AF_UNIX;
    strcpy(listeningAddr.sun_path, SOCKET_PATH);
    unlink(listeningAddr.sun_path);
    len = strlen(listeningAddr.sun_path) + sizeof(listeningAddr.sun_family);
    rc = bind(listeningSocket, (struct sockaddr *)&listeningAddr, len);
    if (rc == -1)
    {
        perror("Couldn't bind socket: ");
        exit(4);
    }
    
    rc = listen(listeningSocket, CONNECTION_QUEUE_SIZE);
    if (rc == -1)
    {
        perror("Couldn't bind socket: ");
        exit(5);
    }
    // END set up listening socket
    
    // Main loop accepting connections and doing the work
    while (true)
    {
        int acceptedSocket;
        struct sockaddr_un remoteAddr;
        socklen_t remoteAddrLen;
        
        printf("Waiting for a connection.\n");
        remoteAddrLen = sizeof(remoteAddr);
        acceptedSocket = accept(listeningSocket, 
                                (struct sockaddr *)&remoteAddr, &remoteAddrLen);
        if (acceptedSocket == -1)
            continue;
        printf("Connection established, waiting for commands.\n");
        
        readCommands(acceptedSocket);
        printf("Connection closed.\n");
        close(acceptedSocket);
    }
    
    return 0;
}
