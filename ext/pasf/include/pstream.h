#ifndef __PSTREAM_H__
#define __PSTREAM_H__

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "pbase.h"
//#define PBLOCK_SIZE (1024 * 8)

enum MSG_TYPE 
{
    P_MSG  = 0, // paio framework   -> msg length + msg data
    CA_MSG = 1, // callagnt commhdr -> msg data (firstType -> int nMsgLen)
    //MG_MSG  = 2, // msg data (firstType -> unsigned int unMagicCookie, secondType -> unsigned int unMsgLen)
    //DEF_MSG = 3  // msg data (firstType -> int nMsgLen)
};

enum STREAM_SIZE
{
   PSTREAM_DEFAULT_SIZE = 2048 * 1024,

};


class PMemStream 
{
public : 
   PMemStream(int size) { 
      _ptr = (char *)malloc(size);
      if(_ptr) {
         _pos = 0;
         _size = size;
      }
      else {
         _pos = 0;
         _size = 0;
         PEPRINT("PMemStream malloc CRIT ERROR size:%d\n", size);
         assert(-1);
      }
   };
   ~PMemStream(){
       if (_ptr) free(_ptr);
    };

   void reset() { _pos = 0; }
   int length() { return _pos; }
   int size() { return _size; } // max memory size

   bool getValueInt(int pos, int & runValue) {
      if(_pos-pos < sizeof(int)) {
         return false;
      }
      char * ptr = _ptr + pos;
      memcpy(&runValue, ptr, sizeof(int));
#if 0
      char * ptr = _ptr + pos;
      runValue  = (int)(*ptr++);
      runValue += (int)(*ptr++) << 8;
      runValue += (int)(*ptr++) << 16;
      runValue += (int)(*ptr++) << 24;
#endif
      return true;
   }
 
   bool remove(int len) {
      int left = _pos - len;
      if(left < 0) {
         PEPRINT("stream remove ERR %d=%d-%d\n", left, _pos, len);
         return false;
      }
      if(left > 0) {
         memmove(_ptr, _ptr+len, left);
      }
      _pos  = left;
   }

   bool read(char * buff, int len) {
      int left = _pos - len;
      if(left < 0) {
         return false;
      }
      memcpy(buff, _ptr, len);
      if(left > 0) {
         memmove(_ptr, _ptr+len, left);
      }
      _pos  = left;
   }

   bool write(const char * buff, int len) {
      int left = _size - _pos;
      if(left < len) {
         int size = ((len - left) / 1024 + 1) * 1024 + _size;   // buffer size : multiple 1024
         printf("## PMemStream::write >  _size=%d, size=%d, _pos=%d, len=%d\n", 
               _size, size, _pos, len);

         char * pTmp = (char *)malloc(size);
         if(!pTmp) {
            printf("PMemStream can't allocsation memory! (size=%d)\n" , size);
            return false;
         }
         _size = size;

         memcpy(pTmp, _ptr, _pos);
         free(_ptr);
         _ptr = pTmp;
         
      } 
      memcpy(_ptr+_pos, buff, len);
      _pos += len; 
      return true;
   }

   const char * ptr(int pos= 0) {
      if(_pos < pos) return NULL;
      return _ptr + pos;
   }


   char * tail() 
   {
   
      return _ptr+_pos;
   }

   bool move(int size) 
   {
      if(_size < (_pos + size)) return false;
      _pos += size;
      return true;
   }

   int left() {
      return _size - _pos; 
   } 
   
private:

   char * _ptr; 

   
   int   _pos;  // used buff
   int   _size; // allocate memory size
};


#endif // __PSTREAM_H__
