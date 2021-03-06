#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <algorithm>

#include <string.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <libpq-fe.h>


extern "C"
{
#include <bcg729/decoder.h>
#include "typedef.h"
#include "codecParameters.h"
}

using namespace std;

const char* usage = "<path-to-payload-dir> <path-to-wav-dir>\n";

typedef struct _wav_file_info {
  string filename;
  long double ts_opened;
  long double ts_closed;
  uint32_t samples; 
} wav_file_info;

bool cmp(wav_file_info a, wav_file_info b) { return (a.ts_opened<b.ts_opened); }

#define PATH_TO_CONF "/data/conf/tap-rtpsave.conf"
/* #define DB_COONNECTION "host=localhost dbname=voiplog user=dbworker password='vFcnbh_+'" */

PGconn* conn;

void exiterror(string mess)
{
  cerr << mess;
  exit(1);
}

map<string,string> LoadConfig(string filename)
{
    ifstream in(filename);
    map<string,string> res;
    while(in)
    {
      string ln;
      getline(in, ln);
      string::size_type p_d=ln.find_first_of('=');
      if( p_d != string::npos ){
         string raw_key=ln.substr(0,p_d);
         string::size_type pk_b = raw_key.find_first_not_of(" \t");
         if( pk_b != string::npos ){
            string::size_type pk_e = raw_key.find_last_not_of(" \t");
            if( pk_e != string::npos ){
               string raw_value =ln.substr(p_d+1,string::npos);
               string::size_type pv_b = raw_value.find_first_not_of(" \t");
               if( pv_b != string::npos ){
                  string::size_type pv_e = raw_value.find_last_not_of(" \t");
                  if( pv_e != string::npos ){
                     res[raw_key.substr(pk_b,pk_e-pk_b+1)] = raw_value.substr(pv_b,pv_e-pv_b+1);
                     string::size_type pos1 = raw_value.find_first_of("\"");
                     string::size_type pos2 = raw_value.find_last_of("\"");
                     if(pos1 != string::npos && pos2 != string::npos && pos2 > pos1) 
                       res[raw_key.substr(pk_b,pk_e-pk_b+1)] = raw_value.substr(pos1+1,pos2-pos1-1);
                  }
               }
            }
         }
      }
    }
    in.close();
    return res;
}

int32_t decodeG729(vector<int16_t> & dest, const vector<unsigned char> & src)
{
  uint8_t inputBuffer[10] = { 0 };
  int framesize = ( src.size() < 8 ) ? 2 : 10;
  uint32_t decodesize = 0;

  //create the decoder 
  bcg729DecoderChannelContextStruct *Decoder = initBcg729DecoderChannel();

  while (decodesize < src.size())
  {
          memcpy(inputBuffer, src.data() + decodesize, framesize);
          decodesize += framesize;
  	
  	framesize = ( src.size() - decodesize < 8 ) ? 2 : 10;

          uint8_t frameErasureFlag1 = 0;
          if ((uint8_t)inputBuffer[0] == 0) //frame has been erased
          {
                  frameErasureFlag1 = 1;
          }

          int16_t tempoutpuBuffer[L_FRAME] = { 0 };
          bcg729Decoder(Decoder, inputBuffer, frameErasureFlag1, tempoutpuBuffer);
          dest.insert(dest.end(), tempoutpuBuffer, tempoutpuBuffer + L_FRAME);

  }
  //release decoder
  closeBcg729DecoderChannel(Decoder);
  return dest.size();
}

#pragma pack(push, 1)
struct WAVHEADER
{
  uint8_t chunkId[4]={0x52,0x49,0x46,0x46};     //"RIFF"
  int32_t chunkSize;                            // length 36 + size(pcm payload)
  uint8_t format[4]={0x57,0x41,0x56,0x45};      //"WAVE"
  //
  uint8_t subchunk1Id[8]={0x66,0x6d,0x74,0x20,0x12,0,0,0}; //"fmt "
  //int32_t subchunk1Size=0x12;  //0x12, moved to subchunk1Id
  int16_t audioFormat;    //7 - u-low(0), 6 - a-low(8),  
  int16_t numChannels;    //1 - mono, 2 -stereo
  int32_t sampleRate;     // 8000Hz, 16000Hz, 44100Hz, ...
  int32_t byteRate;       // sampleRate * numChannels * bitsPerSample/8
  int16_t blockAlign;     //numChannels * bitsPerSample/8
  int16_t bitsPerSample;  // 8bit, 16bit, ...
  //looks like an undocumented RIFF extentions
  uint8_t pad1[10]={0,0,0x66,0x61,0x63,0x74,4,0,0,0}; //"\0\0fact\0\4\0\0\0"
  int32_t pad2;            
  //
  uint8_t subchunk2Id[4]={0x64,0x61,0x74,0x61}; //"data"
  int32_t subchunk2Size; //datasize, numSamples * numChannels * bitsPerSample/8, part2

} wavheader;
#pragma pack(pop)

int file2wav(string& payloaddir, string& filename, string& outputdir, bool debug_output){
  int16_t numChannels=1; 
  int32_t sampleRate=8000;
  int16_t bitsPerSample=8;
  int16_t audioFormat=6;
  vector<int16_t> pRawData;

  unordered_map<string,int16_t> codecs;
  codecs["8"]=6;   // PCMA a-low(8) G711a encoded by 6 in WAV
  codecs["0"]=7;   // PCMA mu-low(0) G711u encoded by 7 in WAV
  codecs["18"]=18; // just trick for G729

  size_t pos=filename.find_last_of('.');
  if(pos==string::npos){
    cerr << "File extention needed to determine the payload codec" << endl;
    return (-1);
  }
  string ext=filename.substr(pos+1); 
  auto search = codecs.find(ext);  
  if(search != codecs.end()) audioFormat=codecs[ext];
  else{ 
    cerr << "The codec " << ext << " unsupported" << endl;
    return -1;
  }
  
  ifstream pcm_input (payloaddir+filename, ifstream::binary | std::ifstream::in);
  string output_name;
  pos=filename.find_last_of('/');
  if(pos==string::npos) output_name = filename; 
  else output_name = filename.substr(pos+1);
  output_name =  outputdir + output_name + ".wav";
  ofstream wav_output (output_name,ofstream::binary);

  if(!pcm_input.is_open()){
  	cerr << "Unable to open input file"<<endl;
       return (-1);
  }
  if(!wav_output.is_open()){
  	cerr << "Unable to open output file"<<endl;
       return (-1);
  }

  pcm_input.seekg(0, pcm_input.end);
  int length = pcm_input.tellg();
  pcm_input.seekg(0, pcm_input.beg);

  char * buffer = new char [length];

  pcm_input.read(buffer,length);
  int32_t bytes_readed = pcm_input.gcount();
  pcm_input.close();
  
  if(audioFormat==18){//g729, convert the payload to pcm, then save pcm  to wav
      std::vector<unsigned char> pcmBuffer;
      pcmBuffer.resize(bytes_readed);
      memcpy((char *) pcmBuffer.data(),buffer,bytes_readed);
      decodeG729(pRawData, pcmBuffer);
      audioFormat=1;
      bitsPerSample=16; 
  }
  //
  //The following block works right on little endian processors
  if(audioFormat==1){ //pcm from g729, get size for wav from the pcm vector
    wavheader.chunkSize=50+pRawData.size()*2; //36was
    wavheader.pad2=pRawData.size()*2;
    wavheader.subchunk2Size=pRawData.size()*2;
  }else{ //g711 , get size for wav from the original payload
    wavheader.chunkSize=50+bytes_readed; //36was
    wavheader.pad2=bytes_readed;
    wavheader.subchunk2Size=bytes_readed;
  }
  wavheader.audioFormat=audioFormat;
  wavheader.numChannels=numChannels; 
  wavheader.sampleRate=sampleRate; 
  wavheader.byteRate=sampleRate * numChannels * bitsPerSample/8;
  wavheader.blockAlign=numChannels*bitsPerSample/8;
  wavheader.bitsPerSample=bitsPerSample;
  //
  wav_output.write((char *) &wavheader,sizeof(wavheader));
  
  if(audioFormat==1) //pcm from g729, save the pcm  to wav
     wav_output.write((char *)pRawData.data(),pRawData.size()*2);    
  else //g711a or g711mu, just save the payload to wav 
     wav_output.write(buffer,bytes_readed);
  
  int numSamples = wavheader.subchunk2Size*8/(wavheader.numChannels*wavheader.bitsPerSample);
  if(debug_output) 
       cout << output_name << " written: "<< wav_output.tellp() << " samples:" << numSamples << endl;
  //SQL
  PGresult* res;
  string sqlrequest = "UPDATE files SET samples="+to_string(numSamples)+" WHERE filename='"+filename+"';";
  res = PQexec(conn,sqlrequest.c_str());
  if (PQresultStatus(res) != PGRES_COMMAND_OK) exiterror(PQresultErrorMessage(res));
  PQclear(res);
  wav_output.close();
  //sql
  delete [] buffer;
  return 0;
}

static void handle_events(int fd, int wd, string &pl_path, string &wav_path, bool debug_output ) {
  char buf[4096]
       __attribute__ ((aligned(__alignof__(struct inotify_event))));

  const struct inotify_event *event;
  ssize_t len;
  char *ptr;

  for (;;) {
    len = read(fd, buf, sizeof(buf));
    if (len == -1 && errno != EAGAIN) {
       perror("read");
       exit(EXIT_FAILURE);
    }
    if (len <= 0) break;
    for (ptr = buf; ptr < buf + len; ptr += sizeof(struct inotify_event) + event->len) {
      event = (const struct inotify_event *) ptr;
      if (event->wd == wd ){
            string filename(event->name);
            file2wav(pl_path,filename,wav_path,debug_output);
      }
    }
  }
}

int main( int argc, char** argv ) {
  string payloadpath;
  string wavpath;

  if (argc > 1) payloadpath=string(argv[1]);
  if (argc > 2) wavpath=string(argv[2]);
  
  map<string,string> conf=LoadConfig(string(PATH_TO_CONF));

  bool debug_output = 0;
  auto it=conf.find("DEBUG");
  if(it!=conf.end()) debug_output=stoi(it->second);

  it=conf.find("DB_COONNECTION");
  if(it==conf.end()) exiterror("Can't find DB_COONNECTION configuration in the conf file "); 
  string db_connection=it->second;
  if(debug_output) cout << "DB:" << db_connection << endl;

  if(argc < 3){
     it=conf.find("PATH_TO_STORAGE");
     if(it==conf.end()) exiterror("Can't find PATH_TO_STORAGE configuration in the conf file ");
     payloadpath= it->second + "/payload/";
     wavpath= it->second + "/wav/";
  } 

  if(payloadpath.at(payloadpath.length()-1)!='/') payloadpath +='/';
  if(wavpath.at(wavpath.length()-1)!='/') wavpath +='/';

  int fd, poll_num, wd;
  struct pollfd fds[1];
  nfds_t nfds;
  
  fd = inotify_init1(IN_NONBLOCK);
  if (fd == -1) {
      perror("inotify_init1");
      return (EXIT_FAILURE);
  }
  
  wd = inotify_add_watch(fd, payloadpath.c_str(), IN_CLOSE_WRITE);
  if(wd == -1) {
      cerr << "Impossible to observe:" << payloadpath << endl;
      perror("inotify_add_watch");
      return (EXIT_FAILURE);
  } 

  nfds = 1;
  fds[0].fd = fd;
  fds[0].events = POLLIN;


  conn = PQconnectdb(db_connection.c_str());

  if (PQstatus(conn) != CONNECTION_OK) exiterror(PQerrorMessage(conn));

  cout << "Wait events."<< endl;
  while(1) {
      poll_num = poll(fds, nfds, -1);
      if (poll_num == -1) {
           if (errno == EINTR)
               continue;
           perror("poll");
           exit(EXIT_FAILURE);
      }
      if (poll_num > 0) {
           if (fds[0].revents & POLLIN) handle_events(fd, wd, payloadpath, wavpath, debug_output);
      }
  }

  if (PQstatus(conn) == CONNECTION_OK) PQfinish(conn);
  return 0;
}


