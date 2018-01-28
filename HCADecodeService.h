#pragma once

#include <thread>
#include <vector>
#include <map>
#include "Semaphore.h"
#include "clHCA.h"

class HCADecodeService
{
public:
    HCADecodeService();
    HCADecodeService(unsigned int num_threads, unsigned int chunksize = 24);
    ~HCADecodeService();
    void cancel_decode(void* ptr);
    std::pair<void*, size_t> decode(const char* hcafilename, unsigned int decodefromsample = 0, unsigned int ciphKey1 = 0xBC731A85, unsigned int ciphKey2 = 0x0002B875, float volume = 1.0f, int mode = 16, int loop = 0);
    void wait_on_request(void* ptr);
    void wait_for_finish();
private:
    void Decode_Thread(int id);
    void Main_Thread();
    clHCA workingfile;
    unsigned int numthreads, numchannels, chunksize;
    void* workingrequest;
    std::thread dispatchthread;
    std::vector<std::thread> worker_threads;
    std::map<std::pair<void*, unsigned int>, clHCA> filelist;
    std::vector<unsigned int> blocks;
    int* workingblocks;
	int cursor;
    Semaphore* workersem;
    Semaphore mainsem, datasem, finsem, wavoutsem;
    std::mutex mutex;
    clHCA::stChannel* channels;
    bool shutdown;
};

