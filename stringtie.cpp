#include "rlink.h"
#ifndef NOTHREADS
#include "GThreads.h"
#endif

//#undef GMEMTRACE //-- comment out to track memory use for GDEBUG in Linux

#ifdef GMEMTRACE
#include "proc_mem.h"
#endif

#define VERSION "1.0.2"

//uncomment this to show DBGPRINT messages (for threads)
//#define DEBUGPRINT 1

#ifdef DEBUGPRINT
#define DBGPRINT(x) GMessage(x)
#define DBGPRINT2(a,b) GMessage(a,b)
#define DBGPRINT3(a,b,c) GMessage(a,b,c)
#define DBGPRINT4(a,b,c,d) GMessage(a,b,c,d)
#define DBGPRINT5(a,b,c,d,e) GMessage(a,b,c,d,e)
#else
#define DBGPRINT(x) 
#define DBGPRINT2(a,b) 
#define DBGPRINT3(a,b,c) 
#define DBGPRINT4(a,b,c,d) 
#define DBGPRINT5(a,b,c,d,e) 
#endif

#define USAGE "StringTie v"VERSION" usage:\n\
 stringtie <input.bam> [-G <guide_gff>] [-l <label>] [-o <out_gtf>] [-p <cpus>]\n\
  [-v] [-a <min_anchor_len>] [-m <min_tlen>] [-j <min_anchor_cov>] [-n sens]\n\
  [-C <coverage_file_name>] [-s <maxcov>] [-c <min_bundle_cov>] [-g <bdist>]\n\
  {-B | -b <dir_path>} [-e]\n\
\nAssemble RNA-Seq alignments into potential transcripts.\n\
 \n\
 Options:\n\
 -G reference annotation to use for guiding the assembly process (GTF/GFF3)\n\
 -l name prefix for output transcripts (default: STRG)\n\
 -f minimum isoform fraction (default: 0.1)\n\
 -m minimum assembled transcript length to report (default 200bp)\n\
 -o output path/file name for the assembled transcripts GTF (default: stdout)\n\
 -a minimum anchor length for junctions (default: 10)\n\
 -j minimum junction coverage (default: 1)\n\
 -t disable trimming of predicted transcripts based on coverage\n\
    (default: coverage trimming is enabled)\n\
 -c minimum reads per bp coverage to consider for transcript assembly (default: 2.5)\n\
 -s coverage saturation threshold; further read alignments will be\n\
    ignored in a region where a local coverage depth of <maxcov> \n\
    is reached (default: 1,000,000);\n\
 -v verbose (log bundle processing details)\n\
 -g gap between read mappings triggering a new bundle (default: 50)\n\
 -C output file with reference transcripts that are covered by reads\n\
 -M fraction of bundle allowed to be covered by multi-hit reads (default:0.95)\n\
 -p number of threads (CPUs) to use (default: 1)\n\
 -B enable output of Ballgown table files which will be created in the\n\
    same directory as the output GTF (requires -G, -o recommended)\n\
 -b enable output of Ballgown table files but these files will be \n\
    created under the directory path given as <dir_path>\n\
 -e only estimates the abundance of given reference transcripts (requires -G)\n\
 "
/* 
 -n sensitivity level: 0,1, or 2, 3, with 3 the most sensitive level (default 0)\n\
 -O disable the coverage saturation limit and use a slower two-pass approach\n\
    to process the input alignments, collapsing redundant reads\n\
 -x disable fast computing for transcript path; default: yes\n\
 -i the reference annotation contains partial transcripts\n\
 -w weight the maximum flow algorithm towards the transcript with higher rate (abundance); default: no\n\
 -y include EM algorithm in max flow estimation; default: no\n\
 -z don't include source in the max flow algorithm\n\
 -P output file with all transcripts in reference that are partially covered by reads
 -M fraction of bundle allowed to be covered by multi-hit reads (paper uses default: 1)\n\
 -c minimum bundle reads per bp coverage to consider for assembly (paper uses default: 3)\n\
 -S more sensitive run (default: no) disabled for now \n\
*/
//---- globals

FILE* f_out=NULL;
FILE* c_out=NULL;
GStr outfname;
GStr out_dir;
GStr tmpfname;
bool guided=false;
bool trim=true;
bool fast=true;
bool eonly=false; // parameter -e
bool specific=false;
bool complete=true;
bool partialcov=false;
int num_cpus=1;
int mintranscriptlen=200; // minimum length for a transcript to be printed
int sensitivitylevel=1;
int junctionsupport=10; // anchor length for junction to be considered well supported <- consider shorter??
int junctionthr=1; // number of reads needed to support a particular junction
float readthr=2.5;     // read coverage per bundle bp to accept it; otherwise considered noise; paper uses 3
uint bundledist=50;  // reads at what distance should be considered part of separate bundles
float mcov=0.95; // fraction of bundle allowed to be covered by multi-hit reads paper uses 1

// different options of implementation reflected with the next three options
bool includesource=true;
bool EM=false;
bool weight=false;

float isofrac=0.1;
GStr label("STRG");
GStr ballgown_dir;

GStr guidegff;

bool debugMode=false;
bool verbose=false;
bool ballgown=false;

int maxReadCov=1000000; //max local read coverage (changed with -s option)
//no more reads will be considered for a bundle if the local coverage exceeds this value
//(each exon is checked for this)

bool singlePass=true; //-O will set this to False

int GeneNo=0; //-- global "gene" counter
unsigned long long int Num_Fragments=0; //global fragment counter (aligned pairs)
unsigned long long int Frag_Len=0;
//bool firstPrint=true; //just for writing the GFF header before the first transcript is printed

GffNames* gseqNames=NULL; //used as a dictionary for genomic sequence names and ids

#ifdef GMEMTRACE
 double maxMemRS=0;
 double maxMemVM=0;
 GStr maxMemBundle;
#endif


#ifndef NOTHREADS

GFastMutex dataMutex; //to manage availability of data records ready to be loaded by main thread
GVec<int> dataClear; //indexes of data bundles cleared for loading by main thread (clear data pool)

GFastMutex waitMutex; // for main program to make sure there are threads ready/waiting
int threadsWaiting=0; // how many worker threads are waiting

GFastMutex printMutex; //for writing the output to file
GFastMutex logMutex; //only when verbose - to avoid mangling the log output
GMutex queueMutex; //whenever bundleQueue is updated
GFastMutex bamReadingMutex;
GConditionVar haveBundles; //will notify all threads when bundles are pushed in the ready queue
                           //or no more bundles are coming

int bundleWork=1; // bit 0 set if bundles are still being prepared (BAM file not exhausted yet)
                  // bit 1 set if there are Bundles ready in the queue
#endif

bool NoMoreBundles=false;
bool moreBundles(); //thread-safe retrieves NoMoreBundles
void noMoreBundles(); //sets NoMoreBundles to true
//--
GStr Process_Options(GArgs* args);
char* sprintTime();

void processBundle(BundleData* bundle);
//void processBundle1stPass(BundleData* bundle); //two-pass testing

#ifndef NOTHREADS

bool waitForThreads(); //wait for at least 1 worker thread to enter "ready" state

void workerThread(GThreadData& td); // Thread function

//check if a worker thread popped the data queue:
bool queuePopped(GPVec<BundleData>& bundleQueue, int prevCount); 

//prepare the next free bundle for loading
int waitForData(BundleData* bundles);
#endif

int main(int argc, char * const argv[]) {

 // == Process arguments.
 GArgs args(argc, argv, 
   //"debug;help;fast;xhvntj:D:G:C:l:m:o:a:j:c:f:p:g:");
   "debug;help;xyzwShvtien:j:s:D:G:C:l:m:o:a:j:c:f:p:g:P:M:Bb:");
 args.printError(USAGE, true);

 GStr bamfname=Process_Options(&args);
 // == Done argument processing.

 GVec<GRefData> refguides; // plain vector with transcripts for each chromosome
 GPVec<RC_ScaffData> refguides_RC_Data(true);
 GPVec<RC_Feature> refguides_RC_exons(true);
 GPVec<RC_Feature> refguides_RC_introns(true);
 GVec<int> alncounts(30,0); //number of read alignments per chromosome [gseq_id]

#ifdef DEBUGPRINT
  verbose=true;
#endif

const char* ERR_BAM_SORT="\nError: the input alignment file is not sorted!\n";

 if(guided) { // read guiding transcripts from input gff file
	 if (verbose) {
		 printTime(stderr);
		 GMessage(" Loading reference annotation (guides)..\n");
	 }
   FILE* f=fopen(guidegff.chars(),"r");
   if (f==NULL) GError("Error: could not open reference annotation file (%s)!\n",
       guidegff.chars());
   //                transcripts_only    sort gffr->gfflst by loc?
   GffReader gffr(f,       true,                   true); //loading only recognizable transcript features
   gffr.showWarnings(verbose);
   //        keepAttrs    mergeCloseExons   noExonAttrs
   gffr.readAll(false,          true,        true);
   //the list of GffObj is in gffr.gflst, sorted by chromosome and start-end coordinates
   //collect them in other data structures, if it's kept for later call gffobj->isUsed(true)
   // (otherwise it'll be deallocated when gffr is destroyed due to going out of scope)
   refguides.setCount(gffr.gseqStats.Count()); //maximum gseqid
   uint cur_tid=0;
   uint cur_exon_id=0;
   uint cur_intron_id=0;
   std::set<RC_ScaffSeg> exons;
   std::set<RC_ScaffSeg> introns;
   //assign unique transcript IDs based on the sorted order
   int last_refid=0;
   for (int i=0;i<gffr.gflst.Count();i++) {
	   GffObj* m=gffr.gflst[i];
	   if (ballgown) {
		   RC_ScaffData* tdata=new RC_ScaffData(*m, ++cur_tid);
		   m->uptr=tdata;
		   if (last_refid!=m->gseq_id) {
			   //chromosome switch
			   exons.clear();
			   introns.clear();
			   last_refid=m->gseq_id;
		   }
		   refguides_RC_Data.Add(tdata);
		   tdata->rc_addFeatures(cur_exon_id, exons, refguides_RC_exons,
				   cur_intron_id, introns, refguides_RC_introns);
	   }

	   GRefData& grefdata = refguides[m->gseq_id];
	   grefdata.add(&gffr, m); //transcripts already sorted by location
   }
	 if (verbose) {
		 printTime(stderr);
		 GMessage(" %d reference transcripts loaded.\n", gffr.gflst.Count());
	 }
	fclose(f);
 }

 // --- here we do the input processing
 gseqNames=GffObj::names; //might have been populated already by gff data
 gffnames_ref(gseqNames);  //initialize the names collection if not guided

 GBamReader bamreader(bamfname.chars());

 GHash<int> hashread;      //read_name:pos:hit_index => readlist index
 //my %hashjunction;  //junction coords and strand => junction index
 // we won't need this because we can quick-search in junction GList directly
 //my @guides=(); //set of annotation transcript for the current locus
 GList<GffObj>* guides=NULL; //list of transcripts on a specific chromosome

 int currentstart=0, currentend=0;
 int ng_start=0;
 int ng_end=-1;
 int ng=0;
 GStr lastref;
 int lastref_id=-1; //last seen gseq_id
 // int ncluster=0; used it for debug purposes only

 //Ballgown files
 FILE* f_tdata=NULL;
 FILE* f_edata=NULL;
 FILE* f_idata=NULL;
 FILE* f_e2t=NULL;
 FILE* f_i2t=NULL;
if (ballgown)
 Ballgown_setupFiles(f_tdata, f_edata, f_idata, f_e2t, f_i2t);
#ifndef NOTHREADS
 GThread* threads=new GThread[num_cpus];
 GPVec<BundleData> bundleQueue(false);
 BundleData* bundles=new BundleData[num_cpus+1]; //extra one being prepared while all others are processed
 dataClear.setCapacity(num_cpus+1);
 for (int b=0;b<num_cpus;b++) {
	 threads[b].kickStart(workerThread, (void*) &bundleQueue);
	 bundles[b+1].idx=b+1;
	 dataClear.Push(b);
   }
 BundleData* bundle = &(bundles[num_cpus]);
#else
 BundleData bundles[1];
 BundleData* bundle = &(bundles[0]);
#endif
 GBamRecord* brec=NULL;
 bool more_alns=true;
 int prev_pos=0;
 while (more_alns) {
	 bool chr_changed=false;
	 int pos=0;
	 const char* rname=NULL;
	 char strand=0;
	 char xstrand=0;
	 int nh=1;
	 int hi=0;
	 int gseq_id=lastref_id;  //current chr id
	 bool new_bundle=false;
	 delete brec;
	 if ((brec=bamreader.next())!=NULL) {
		 if (brec->isUnmapped()) continue;
		 rname=brec->refName();
		 if (rname==NULL) GError("Error: cannot retrieve target seq name from BAM record!\n");
		 pos=brec->start; //BAM is 0 based, but GBamRecord makes it 1-based
		 chr_changed=(lastref.is_empty() || lastref!=rname);
		 if (chr_changed) {
			 gseq_id=gseqNames->gseqs.addName(rname);
			 if (alncounts.Count()<=gseq_id) {
				 alncounts.Resize(gseq_id+1, 0);
			 }
			 else if (alncounts[gseq_id]>0) GError(ERR_BAM_SORT);
			 prev_pos=0;
		 }
		 if (pos<prev_pos) GError(ERR_BAM_SORT);
		 alncounts[gseq_id]++;
		 prev_pos=pos;
		 xstrand=brec->spliceStrand();
		 if (xstrand=='+') strand=1;
		 else if (xstrand=='-') strand=-1;
		 nh=brec->tag_int("NH");
		 if (nh==0) nh=1;
		 hi=brec->tag_int("HI");
		 if (!chr_changed && currentend>0 && pos>currentend+(int)bundledist)
			   new_bundle=true;
	 }
	 else { //no more alignments
		 more_alns=false;
		 new_bundle=true; //fake a new start (end of last bundle)
	 }
	 if (new_bundle || chr_changed) {
		 hashread.Clear();
		 if (bundle->readlist.Count()>0) { // process reads in previous bundle
			 if (guides && ng_end>=ng_start) {
				 for (int gi=ng_start;gi<=ng_end;gi++)
					 bundle->keepguides.Add((*guides)[gi]);
			 }
			// geneno=infer_transcripts(geneno, lastref, $label,\@readlist,$readthr,\@junction,$junctionthr,$mintranscriptlen,\@keepguides);
			// (readthr, junctionthr, mintranscriptlen are globals)
			/* if (ballgown && bundle->rc_data) {
				bundle->rc_data->setupFiles(f_tdata, f_edata, f_idata, f_e2t, f_i2t);
			}*/
			bundle->getReady(currentstart, currentend);
#ifndef NOTHREADS
			//push this in the bundle queue, where it'll be picked up by the threads
			DBGPRINT2("##> Locking queueMutex to push loaded bundle into the queue (bundle.start=%d)\n", bundle->start);
			queueMutex.lock();
			bundleQueue.Push(bundle);
			bundleWork |= 0x02; //set bit 1
			int qCount=bundleQueue.Count();
			queueMutex.unlock();
			do {
			     waitForThreads();
			     DBGPRINT("##> NOTIFY any thread...\n");
			     haveBundles.notify_one();
			     //this_thread::sleep_for(chrono::milliseconds(1));
			     sleep(0);
			} while (!queuePopped(bundleQueue, qCount));
#else //no threads
			Num_Fragments+=bundle->num_fragments;
			Frag_Len+=bundle->frag_len;
			processBundle(bundle);
#endif
			// ncluster++; used it for debug purposes only
		 } //have alignments to process
		 else { //no read alignments in this bundle?
			bundle->Clear();
#ifndef NOTHREADS
	dataMutex.lock();
	dataClear.Push(bundle->idx);
	dataMutex.unlock();
#endif
		 }

		 if (chr_changed) {
			 if (guided) {
				 ng=0;
				 guides=NULL;
				 ng_start=0;
				 ng_end=-1;
				 if (refguides.Count()>gseq_id && refguides[gseq_id].rnas.Count()>0) {
					 guides=&(refguides[gseq_id].rnas);
					 ng=guides->Count();
				 }
			 }
			 lastref=rname;
			 lastref_id=gseq_id;
			 currentend=0;
		 }

		 if (!more_alns) {
				if (verbose) {
#ifndef NOTHREADS
						GLockGuard<GFastMutex> lock(logMutex);
#endif
					printTime(stderr);
					GMessage(" %llu aligned fragments found.\n", Num_Fragments);
					//GMessage(" Done reading alignments.\n");
				}
			 noMoreBundles();
			 break;
		 }
#ifndef NOTHREADS
		 int new_bidx=waitForData(bundles);
		 if (new_bidx<0) {
			 //should never happen!
			 GError("Error: waitForData() returned invalid bundle index(%d)!\n",new_bidx);
			 break;
		 }
		 bundle=&(bundles[new_bidx]);
#endif
		 currentstart=pos;
		 currentend=brec->end;
		 if (guides) { //guided and guides!=NULL
			 ng_start=ng_end+1;
			 while (ng_start<ng && (int)(*guides)[ng_start]->end < pos) { ng_start++; } // skip guides that have no read coverage
			 //if(ng_start<ng && (int)(*guides)[ng_start]->start<pos) {
			 int ng_ovlstart=ng_start;
			 //add all guides overlapping the current read
			 while (ng_ovlstart<ng && (int)(*guides)[ng_ovlstart]->start<=currentend) {
				 if (currentstart>(int)(*guides)[ng_ovlstart]->start)
					 currentstart=(*guides)[ng_ovlstart]->start;
				 if (currentend<(int)(*guides)[ng_ovlstart]->end)
					 currentend=(*guides)[ng_ovlstart]->end;
				 if (ballgown) bundle->rc_store_t((*guides)[ng_ovlstart]);
				 ng_ovlstart++;
			 }
			 if (ng_ovlstart>ng_start) ng_end=ng_ovlstart-1;
				 /*
				 while(ng_end+1<ng && (int)(*guides)[ng_end+1]->start<=pos) {
					 ng_end++;
					 if(currentend<(int)(*guides)[ng_end]->end) {
						 currentend=(*guides)[ng_end]->end;
					 }
				 }
				 */
		 } //guides present on the current chromosome
		bundle->refseq=lastref;
		bundle->start=currentstart;
		bundle->end=currentend;
	 } //<---- new bundle
	 //currentend=process_read(currentstart, currentend, bundle->readlist, hashread,
		//	 bundle->junction, *brec, strand, nh, hi, bundle->bpcov);
     //currentend=
	 if (currentend<(int)brec->end) {
		 //current read just pushed upper boundary of the bundle
		 //this might never happen if a longer guide was added already to the bundle
		 currentend=brec->end;
		 if (guides) { //add any newly overlapping guides to bundle
			 bool cend_changed;
			 do {
				 cend_changed=false;
				 while (ng_end+1<ng && (int)(*guides)[ng_end+1]->start<=currentend) {
					 ng_end++;
					 //more transcripts overlapping this bundle
					 if (ballgown) bundle->rc_store_t((*guides)[ng_end]);
					 if(currentend<(int)(*guides)[ng_end]->end) {
						 currentend=(*guides)[ng_end]->end;
						 cend_changed=true;
					 }
				 }
			 } while (cend_changed);
		 }
	 } //adjusted currentend and checked for overlapping reference transcripts
     bool ref_overlap=false;
	 if (ballgown && bundle->rc_data) ref_overlap=bundle->rc_count_hit(*brec, xstrand, nh);
	 countRead(*bundle, *brec, hi);
	 if (!ballgown || ref_overlap) {
	    processRead(currentstart, currentend, *bundle, hashread, *brec, strand, nh, hi);
	 }
   //update current end to be at least as big as the start of the read pair in the fragment?? -> maybe not because then I could introduce some false positives with paired reads mapped badly

	 /*
	 if(guides) { // I need to adjust end according to guides
		 while( ng_end+1 < ng && (int)(*guides)[ng_end+1]->start<=currentend) {
			 ng_end++;
			 if(currentend < (int)(*guides)[ng_end]->end) {
				 currentend=(*guides)[ng_end]->end;
			 }
		 }
	 }
	 */
 } //for each read alignment

 //cleaning up
 delete brec;
 bamreader.bclose();
#ifndef NOTHREADS
 for (int t=0;t<num_cpus;t++)
	 threads[t].join();
 if (verbose) {
   printTime(stderr);
   GMessage(" All threads finished.\n");
 }
 delete[] threads;
 delete[] bundles;
#else
 if (verbose) {
    printTime(stderr);
    GMessage(" Done.\n");
 }
#endif

 //if (f_out && f_out!=stdout) fclose(f_out);
 fclose(f_out);

 // write the FPKMs

 if(verbose) {
	 GMessage("Total count of aligned fragments: %llu\n",Num_Fragments);
	 //GMessage("Fragment length:%llu\n",Frag_Len);
	 GMessage("Average fragment length:%g\n",(float)Frag_Len/Num_Fragments);
 }

 f_out=stdout;
 if(outfname!="stdout") {
	 f_out=fopen(outfname.chars(), "w");
	 if (f_out==NULL) GError("Error creating output file %s\n", outfname.chars());
 }
 FILE* t_out=fopen(tmpfname.chars(),"rt");
 if (t_out!=NULL) {
	 char* linebuf=NULL;
	 int linebuflen=5000;
     GMALLOC(linebuf, linebuflen);
	 int nl;
	 int tlen;
	 float tcov;
	 float fpkm;
	 float calc_fpkm;
	 int t_id;
	 while(fgetline(linebuf,linebuflen,t_out)) {
		 sscanf(linebuf,"%d %d %d %g %g", &nl, &tlen, &t_id, &fpkm, &tcov);
		 calc_fpkm=tcov*1000000000/Frag_Len;
		 if (ballgown && t_id>0) {
			 refguides_RC_Data[t_id-1]->fpkm=calc_fpkm;
			 refguides_RC_Data[t_id-1]->cov=tcov;
		 }
		 for(int i=0;i<nl;i++) {
			 fgetline(linebuf,linebuflen,t_out);
			 if(!i) {
				 //linebuf[strlen(line)-1]='\0';
				 fprintf(f_out,"%s",linebuf);
				 fprintf(f_out,"FPKM \"%.6f\";",calc_fpkm);
				 //fprintf(f_out,"FPKM \"%.6f\"; calculated_FPKM \"%.6f\";",tcov*1000000000/Frag_Len,fpkm*1000000000/(Num_Fragments*tlen));
				 //fprintf(f_out,"flen \"%.6f\"; FPKM \"%.6f\";",fpkm,fpkm*1000000000/Num_Fragments);
				 fprintf(f_out,"\n");
			 }
			 else fprintf(f_out,"%s\n",linebuf);
		 }
	 }
	 fclose(f_out);
	 fclose(t_out);
	 GFREE(linebuf);
	 remove(tmpfname.chars());
 }
 else {
	 fclose(f_out);
	 GError("No temporary file %s present!\n",tmpfname.chars());
 }

 //lastly, for ballgown, rewrite the tdata file with updated cov and fpkm
 if (ballgown) {
	 rc_writeRC(refguides_RC_Data, refguides_RC_exons, refguides_RC_introns,
			 f_tdata, f_edata, f_idata, f_e2t, f_i2t);
 }

 gffnames_unref(gseqNames); //deallocate names collection


#ifdef GMEMTRACE
 if(verbose) GMessage(" Max bundle memory: %6.1fMB for bundle %s\n", maxMemRS/1024, maxMemBundle.chars());
#endif
} // -- END main

//----------------------------------------
char* sprintTime() {
	static char sbuf[32];
	time_t ltime; /* calendar time */
	ltime=time(NULL);
	struct tm *t=localtime(&ltime);
	sprintf(sbuf, "%02d_%02d_%02d:%02d:%02d",t->tm_mon+1, t->tm_mday,
			t->tm_hour, t->tm_min, t->tm_sec);
	return(sbuf);
}


GStr Process_Options(GArgs* args) {

	if (args->getOpt('h') || args->getOpt("help")) {
		GMessage("%s",USAGE);
	    exit(1);
	}

	 debugMode=(args->getOpt("debug")!=NULL || args->getOpt('D')!=NULL);
	 fast=!(args->getOpt('x')!=NULL);
	 verbose=(args->getOpt('v')!=NULL);
	 if (verbose) {
	     fprintf(stderr, "Command line was:\n");
	     args->printCmdLine(stderr);
	 }
	 complete=!(args->getOpt('i')!=NULL);
	 trim=!(args->getOpt('t')!=NULL);
	 includesource=!(args->getOpt('z')!=NULL);
	 EM=(args->getOpt('y')!=NULL);
	 weight=(args->getOpt('w')!=NULL);
	 GStr s=args->getOpt('m');
	 if (!s.is_empty()) mintranscriptlen=s.asInt();

	 s=args->getOpt('n');
	 if (!s.is_empty()) {
		 sensitivitylevel=s.asInt();
		 if(sensitivitylevel<0) {
			 sensitivitylevel=0;
			 fprintf(stderr,"sensitivity level out of range: setting sensitivity level at 0\n");
		 }
		 if(sensitivitylevel>3) {
			 sensitivitylevel=3;
			 fprintf(stderr,"sensitivity level out of range: setting sensitivity level at 2\n");
		 }
	 }

	 s=args->getOpt('g');
	 if (!s.is_empty()) bundledist=s.asInt();
	 s=args->getOpt('p');
	 if (!s.is_empty()) {
		   num_cpus=s.asInt();
		   if (num_cpus<=0) num_cpus=1;
	 }

	 s=args->getOpt('a');
	 if (!s.is_empty()) junctionsupport=s.asInt();
	 s=args->getOpt('j');
	 if (!s.is_empty()) junctionthr=s.asInt();
	 s=args->getOpt('c');
	 if (!s.is_empty()) readthr=(float)s.asDouble();
	 s=args->getOpt('l');
	 if (!s.is_empty()) label=s;
	 s=args->getOpt('f');
	 if (!s.is_empty()) {
		 isofrac=(float)s.asDouble();
		 if(isofrac>=1) GError("Miminum isoform fraction (-f coefficient: %f) needs to be less than 1\n",isofrac);
	 }
	 s=args->getOpt('M');
	 if (!s.is_empty()) {
		 mcov=(float)s.asDouble();
	 }

	 //f_out=stdout;
	 tmpfname=args->getOpt('o');
	 outfname="stdout";
	 out_dir="./";
	 if (!tmpfname.is_empty() && tmpfname!="-") {
		 outfname=tmpfname;
		 int pidx=outfname.rindex('/');
		 if (pidx>=0) //path given
			 out_dir=outfname.substr(0,pidx+1);
	 }
	 else { // stdout
		tmpfname=outfname;
		char *stime=sprintTime();
		tmpfname+='.';
		tmpfname+=stime;
	 }
	 if (out_dir!="./") {
		 if (fileExists(out_dir.chars())==0) {
			//directory does not exist, create it
			Gmkdir(out_dir.chars());
		 }
	 }
	 tmpfname+=".tmp";
	 f_out=fopen(tmpfname.chars(), "w");
	 if (f_out==NULL) GError("Error creating output file %s\n", tmpfname.chars());

     /*
	 if (args->getOpt('O')) {
		 singlePass=false;
		 maxReadCov=0;
	 }
     */

	 if (args->getOpt('S')) {
		 sensitivitylevel=2;
	 }

	 s=args->getOpt('s');
	 if (!s.is_empty()) {
		 int r=s.asInt();
		 if (r<2) {
			 GMessage("Warning: invalid -s value, setting coverage saturation threshold, using default (%d)\n", maxReadCov);
		 }
		 else maxReadCov=r;
	 }
	 /*
	 {//DEBUG ONLY:
		 GStr fname(outfname);
		 fname+=".reads";
		 unlink(fname.chars());
	 }
	 */

	 if (args->getOpt('G')) {
	   guidegff=args->getOpt('G');
	   if (fileExists(guidegff.chars())>1)
	        guided=true;
	   else GError("Error: reference annotation file (%s) not found.\n",
	             guidegff.chars());
	 }

	 eonly=(args->getOpt('e')!=NULL);
	 if(eonly && !guided)
		 GError("Error: invalid -e usage, GFF reference not given (-G option required).\n");

	 ballgown_dir=args->getOpt('b');
	 ballgown=(args->getOpt('B')!=NULL);
	 if (ballgown && !ballgown_dir.is_empty()) {
		 GError("Error: please use either -B or -b <path> options, not both.");
	 }
	 if (ballgown) ballgown_dir=out_dir;
	 else if (!ballgown_dir.is_empty()) {
		    ballgown=true;
		    ballgown_dir.chomp('/');ballgown_dir+='/';
			if (fileExists(ballgown_dir.chars())==0) {
				//directory does not exist, create it
				Gmkdir(ballgown_dir.chars());
			}
	 	  }
	 if (ballgown && !guided)
		 GError("Error: invalid -B/-b usage, GFF reference not given (-G option required).\n");

	 s=args->getOpt('P');
	 if (!s.is_empty()) {
		 if(!guided) GError("Error: option -G with reference annotation file has to be specified.\n");
		 c_out=fopen(s.chars(), "w");
		 if (c_out==NULL) GError("Error creating output file %s\n", s.chars());
		 partialcov=true;
	 }
	 else {
		 s=args->getOpt('C');
		 if (!s.is_empty()) {
			 if(!guided) GError("Error: invalid -C usage, GFF reference not given (-G option required).\n");
			 c_out=fopen(s.chars(), "w");
			 if (c_out==NULL) GError("Error creating output file %s\n", s.chars());
			 num_cpus=1;
		 }
	 }

	 int numbam=args->startNonOpt();
	 if (numbam==0 || numbam>1) {
	 	 GMessage("%s\nError: no BAM input file provided!\n",USAGE);
	 	 exit(1);
	 }

	 s=args->nextNonOpt();

	 return(s);
}

//---------------
bool moreBundles() { //getter (interogation)
	bool v=true;
#ifndef NOTHREADS
  GLockGuard<GFastMutex> lock(bamReadingMutex);
#endif
  v = ! NoMoreBundles;
  return v;
}

void noMoreBundles() { //setter
#ifndef NOTHREADS
		bamReadingMutex.lock();
		NoMoreBundles=true;
		bamReadingMutex.unlock();
		queueMutex.lock();
		bundleWork &= ~(int)0x01; //clear bit 0;
		queueMutex.unlock();
		bool areThreadsWaiting=true;
		do {
		  waitMutex.lock();
		   areThreadsWaiting=(threadsWaiting>0);
		  waitMutex.unlock();
		  if (areThreadsWaiting) {
		    DBGPRINT("##> NOTIFY ALL workers: no more data!\n");
		    haveBundles.notify_all();
		    this_thread::sleep_for(chrono::milliseconds(30));
		    waitMutex.lock();
		     areThreadsWaiting=(threadsWaiting>0);
		    waitMutex.unlock();
		    this_thread::sleep_for(chrono::milliseconds(30));
		  }
		} while (areThreadsWaiting); //paranoid check that all threads stopped waiting
#else
	  NoMoreBundles=true;
#endif
}

/*
void processBundle1stPass(BundleData* bundle) {
	// code executed on bundle data after 1st pass
	//bundle->readlist() is empty here also no pairs data are available
	//just splice sites and coverage info for the bundle
	//TODO: build the groups here (bundle->groups) so processRead() can
	//collapse reads efficiently in 2nd pass
	if (verbose) {
		printTime(stderr);
		GMessage(">bundle %s:%d-%d(%d) (%djs) begins 1st pass processing...\n",
				bundle->refseq.chars(), bundle->start, bundle->end,
				bundle->numreads, bundle->junction.Count());
	}
	// generate groups here, storing them in some bundle->groups data structure
	if (verbose) {
//	#ifndef NOTHREADS
//			GLockGuard<GFastMutex> lock(logMutex);
//	#endif
	  printTime(stderr);
	  GMessage("^bundle %s:%d-%d(%d) 1st pass done.\n",bundle->refseq.chars(),
	  		bundle->start, bundle->end, bundle->numreads);
	}
}

*/

/*
void processBundle(BundleData* bundle) {
	if (verbose) {
	#ifndef NOTHREADS
			GLockGuard<GFastMutex> lock(logMutex);
	#endif
		printTime(stderr);
		GMessage(">bundle %s:%d-%d(%d) (%djs) begins processing...\n",
				bundle->refseq.chars(), bundle->start, bundle->end, bundle->numreads, bundle->junction.Count());
#ifdef GMEMTRACE
		double vm,rsm;
		get_mem_usage(vm, rsm);
		GMessage(" memory usage: %6.1fMB\n",rsm/1024);
		if (rsm>maxMemRS) {
			maxMemRS=rsm;
			maxMemVM=vm;
			maxMemBundle.format("%s:%d-%d(%d)", bundle->refseq.chars(), bundle->start, bundle->end, bundle->numreads);
		}
#endif
	}
	int ngenes=infer_transcripts(bundle->start,bundle->readlist,
	        bundle->junction, bundle->keepguides, bundle->bpcov, bundle->pred);
	if (bundle->pred.Count()>0) {
#ifndef NOTHREADS
		GLockGuard<GFastMutex> lock(printMutex);
#endif
		GeneNo=print_transcripts(bundle->pred, ngenes, GeneNo, bundle->refseq);
	}
	if (verbose) {
	#ifndef NOTHREADS
			GLockGuard<GFastMutex> lock(logMutex);
	#endif
	  printTime(stderr);
	  GMessage("^bundle %s:%d-%d(%d) done (%d processed potential transcripts).\n",bundle->refseq.chars(),
	  		bundle->start, bundle->end, bundle->numreads, bundle->pred.Count());
	}
	bundle->Clear(); //full clear (after the 2nd pass unless singlePass was requested)
#ifndef NOTHREADS
	dataMutex.lock();
	dataClear.Push(bundle->idx);
	dataMutex.unlock();
#endif
}

*/

void processBundle(BundleData* bundle) {
	if (verbose) {
	#ifndef NOTHREADS
			GLockGuard<GFastMutex> lock(logMutex);
	#endif
		printTime(stderr);
		GMessage(">bundle %s:%d-%d(%d) (%djs, %d guides) loaded, begins processing...\n",
				bundle->refseq.chars(), bundle->start, bundle->end, bundle->numreads, 
                bundle->junction.Count(), bundle->keepguides.Count());
#ifdef GMEMTRACE
		double vm,rsm;
		get_mem_usage(vm, rsm);
		GMessage("\t\tstart memory usage: %6.1fMB\n",rsm/1024);
		if (rsm>maxMemRS) {
			maxMemRS=rsm;
			maxMemVM=vm;
			maxMemBundle.format("%s:%d-%d(%d)", bundle->refseq.chars(), bundle->start, bundle->end, bundle->readlist.Count());
		}
#endif
	}
	int ngenes=infer_transcripts(bundle, fast | bundle->covSaturated);
	if (bundle->rc_data) {
		//rc_write_counts(refname.chars(), *bundleData);
		rc_update_exons(*(bundle->rc_data));
	}
	if (bundle->pred.Count()>0) {
#ifndef NOTHREADS
		GLockGuard<GFastMutex> lock(printMutex);
#endif
		GeneNo=printResults(bundle, ngenes, GeneNo, bundle->refseq);
	}
	if (verbose) {
	#ifndef NOTHREADS
			GLockGuard<GFastMutex> lock(logMutex);
	#endif
	  printTime(stderr);
	  GMessage("^bundle %s:%d-%d(%d) done (%d processed potential transcripts).\n",bundle->refseq.chars(),
	  		bundle->start, bundle->end, bundle->readlist.Count(), bundle->pred.Count());
	#ifdef GMEMTRACE
		    double vm,rsm;
		    get_mem_usage(vm, rsm);
		    GMessage("\t\tfinal memory usage: %6.1fMB\n",rsm/1024);
		    if (rsm>maxMemRS) {
			    maxMemRS=rsm;
			    maxMemVM=vm;
			    maxMemBundle.format("%s:%d-%d(%d)", bundle->refseq.chars(), bundle->start, bundle->end, bundle->readlist.Count());
		    }
	#endif
	    }
	bundle->Clear();
#ifndef NOTHREADS
	dataMutex.lock();
	dataClear.Push(bundle->idx);
	dataMutex.unlock();
#endif
}

#ifndef NOTHREADS

bool waitForThreads() {
	bool noneWaiting=true;
	DBGPRINT("##> waiting for workers to enter wait state..\n");
	while (noneWaiting) {
	  waitMutex.lock();
	  noneWaiting=(threadsWaiting<1);
	  waitMutex.unlock();
	  if (noneWaiting)
	    this_thread::sleep_for(chrono::milliseconds(30));
	}
 DBGPRINT("##> there are workers ready now.\n");
 return(!noneWaiting);
}


void workerThread(GThreadData& td) {
	GPVec<BundleData>* bundleQueue = (GPVec<BundleData>*)td.udata;
	//wait for a ready bundle in the queue, until there is no hope for incoming bundles
	DBGPRINT2("---->> Thread%d starting..\n",td.thread->get_id());
	DBGPRINT2("---->> Thread%d locking queueMutex..\n",td.thread->get_id());
	queueMutex.lock(); //enter wait-for-notification loop
	while (bundleWork) {
		DBGPRINT3("---->> Thread%d: waiting.. (queue len=%d)\n",td.thread->get_id(), bundleQueue->Count());
		waitMutex.lock();
		 threadsWaiting++;
		waitMutex.unlock();
		haveBundles.wait(queueMutex);
		waitMutex.lock();
		 if (threadsWaiting>0) threadsWaiting--;
		waitMutex.unlock();
		DBGPRINT3("---->> Thread%d: awakened! (queue len=%d)\n",td.thread->get_id(),bundleQueue->Count());
		BundleData* readyBundle=NULL;
		if ((bundleWork & 0x02)!=0 && (readyBundle=bundleQueue->Pop())!=NULL) { //is bit 1 set?
			 //while ()!=NULL) {
				if (bundleQueue->Count()==0)
					 bundleWork &= ~(int)0x02; //clear bit 1 (queue is empty)
				Num_Fragments+=readyBundle->num_fragments;
				Frag_Len+=readyBundle->frag_len;
				queueMutex.unlock();
				processBundle(readyBundle);
				DBGPRINT2("---->> Thread%d processed bundle, now locking back queueMutex\n", td.thread->get_id());
				queueMutex.lock();
				DBGPRINT2("---->> Thread%d locked back queueMutex\n", td.thread->get_id());
			// }
		}
	} //while there is reason to live
	queueMutex.unlock();
	DBGPRINT2("---->> Thread%d DONE.\n", td.thread->get_id());
}

bool queuePopped(GPVec<BundleData>& bundleQueue, int prevCount) {
  int c;
  queueMutex.lock();
   c=bundleQueue.Count();
  queueMutex.unlock();
  DBGPRINT3("##> post-notification check: qlen is now %d (was %d)\n", c, prevCount);
  return (c==0 || c<prevCount);
}

//prepare the next free bundle for loading
int waitForData(BundleData* bundles) {
	int bidx=-1;
	while (bidx<0) {
	  dataMutex.lock();
	  if (dataClear.Count()>0) {
	    bidx=dataClear.Pop();
	    bundles[bidx].status=BUNDLE_STATUS_LOADING;
	    dataMutex.unlock();
	    return bidx;
	    }
	  dataMutex.unlock();
	  this_thread::sleep_for(chrono::milliseconds(20));
	}
	return -1; // should NEVER happen
}

#endif
