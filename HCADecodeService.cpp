#include <map>
#include "HCADecodeService.h"
#include "clHCA.h"

HCADecodeService::HCADecodeService()
	: numthreads{ std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 1 },
	  mainsem{ this->numthreads },
	  wavoutsem{ this->numthreads },
	  workingblocks{ new int[this->numthreads] },
	  worker_threads{ new std::thread[this->numthreads] },
	  workersem{ new Semaphore[this->numthreads]{} },
	  channels{ new clHCA::stChannel[0x10 * this->numthreads] },
	  chunksize{ 16 },
	  datasem{ 0 },
	  numchannels{ 0 },
	  workingrequest{ nullptr },
	  shutdown{ false }
{
    for (unsigned int i = 0; i < this->numthreads; ++i)
    {
		worker_threads[i] = std::thread{ &HCADecodeService::Decode_Thread, this, i };
        workingblocks[i] = -1;
    }
    dispatchthread = std::thread{ &HCADecodeService::Main_Thread, this };
}

HCADecodeService::HCADecodeService(unsigned int numthreads, unsigned int chunksize)
    : numthreads{ numthreads ? numthreads : (std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 1) },
	  mainsem{ this->numthreads },
	  wavoutsem{ this->numthreads },
	  workingblocks{ new int[this->numthreads] },
	  worker_threads{ new std::thread[this->numthreads] },
	  workersem{ new Semaphore[this->numthreads]{} },
	  channels{ new clHCA::stChannel[0x10 * this->numthreads] },
	  chunksize{ chunksize ? chunksize : 16 },
	  datasem{ 0 },
	  numchannels{ 0 },
	  workingrequest{ nullptr },
	  shutdown{ false }
{
    for (unsigned int i = 0; i < this->numthreads; ++i)
    {
		worker_threads[i] = std::thread{ &HCADecodeService::Decode_Thread, this, i };
        workingblocks[i] = -1;
    }
    dispatchthread = std::thread{ &HCADecodeService::Main_Thread, this };
}

HCADecodeService::~HCADecodeService()
{
    shutdown = true;
    datasem.notify();
    dispatchthread.join();
    delete[] channels;
	delete[] workersem;
	delete[] worker_threads;
}

void HCADecodeService::cancel_decode(void* ptr)
{
	if (ptr == nullptr)
	{
		return;
	}
    filelistmtx.lock();
	auto it = filelist.find(ptr);
	if (it != filelist.end() && it->first == ptr)
	{
		filelist.erase(it);
		datasem.wait();
	}
	filelistmtx.unlock();
    if (workingrequest == ptr)
    {
        workingrequest = nullptr;
    }
    for(unsigned int i = 0; i < numthreads; ++i)
    {
        wavoutsem.wait();
    }
    for(unsigned int i = 0; i < numthreads; ++i)
    {
        wavoutsem.notify();
    }
}

void HCADecodeService::wait_on_request(void* ptr)
{
	if (ptr == nullptr)
	{
		return;
	}
	while (true)
	{
		filelistmtx.lock();
		auto it = filelist.find(ptr);
		if (it != filelist.end() && it->first == ptr)
		{
			filelistmtx.unlock();
			workingmtx.lock();
			workingmtx.unlock();
		}
		else
		{
			filelistmtx.unlock();
			break;
		}
	}
    if(workingrequest == ptr)
    {
		workingmtx.lock();
		workingmtx.unlock();
    }
}

void HCADecodeService::wait_for_finish()
{
    filelistmtx.lock();
    while(!filelist.empty() || workingrequest != nullptr)
    {
        filelistmtx.unlock();
		workingmtx.lock();
		workingmtx.unlock();
        filelistmtx.lock();
    }
    filelistmtx.unlock();
}

std::pair<void*, size_t> HCADecodeService::decode(const char* hcafilename, unsigned int decodefromsample, unsigned int ciphKey1, unsigned int ciphKey2, float volume, int mode, int loop)
{
    clHCA hca(ciphKey1, ciphKey2);
    void* wavptr = nullptr;
    size_t sz = 0;
    hca.Analyze(wavptr, sz, hcafilename, volume, mode, loop);
    if (wavptr != nullptr)
    {
		int decodefromblock = decodefromsample / (hca.get_channelCount() << 10);
        filelistmtx.lock();
        filelist[wavptr].first = std::move(hca);
		filelist[wavptr].second = decodefromblock;
        filelistmtx.unlock();
        datasem.notify();
    }
    return std::pair<void*, size_t>(wavptr, sz);
}

void HCADecodeService::Main_Thread()
{
    while (true)
    {
        datasem.wait();
        if (shutdown)
        {
            break;
        }
        filelistmtx.lock();
		workingmtx.lock();
        auto it = filelist.begin();
        workingrequest = it->first;
        workingfile = std::move(it->second.first);
        unsigned blocknum = it->second.second;
        filelist.erase(it);
		filelistmtx.unlock();
        numchannels = workingfile.get_channelCount();
        workingfile.PrepDecode(channels, numthreads);
        unsigned int blockCount = workingfile.get_blockCount();
        // initiate playback right away
		int sz = blockCount / chunksize + (blockCount % chunksize != 0);
		unsigned int lim = sz * chunksize + blocknum;
        for (unsigned int i = (blocknum/chunksize)*chunksize; i < lim; i += chunksize)
        {
            blocks.push_back(i % blockCount);
        }
		while(!blocks.empty())
		{
			mainsem.wait();
			for (unsigned int i = 0; i < numthreads; ++i)
			{
				if (workingblocks[i] == -1 && !blocks.empty())
				{
					workingblocks[i] = blocks.front();
					blocks.pop_front();
					workersem[i].notify();
				}
			}
			mainsem.notify();
        }
		for (unsigned int i = 0; i < numthreads; ++i)
		{
			while (workingblocks[i] != -1); // busy wait for threads
		}
		workingrequest = nullptr;
		workingmtx.unlock();
    }
    for (unsigned int i = 0; i < numthreads; ++i)
    {
        workersem[i].notify();
        worker_threads[i].join();
    }
}

void HCADecodeService::Decode_Thread(int id)
{
    workersem[id].wait();
    while (workingblocks[id] != -1)
    {
        mainsem.wait();
        workingfile.AsyncDecode(channels + (id * numchannels), workingblocks[id], workingrequest, chunksize, wavoutsem);
        workingblocks[id] = -1;
        mainsem.notify();
        workersem[id].wait();
    }
}
