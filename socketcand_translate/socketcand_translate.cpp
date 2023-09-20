#include "socketcand_translate.h"

int socketcand_translate_state = SOCKETCAND_TRANSLATE_CLOSED;

/*  Used to translate raw CAN data into Socketcand frames
    PARAMETERS:
        buf: buffer to write socketcand frame to
        bufsize: size of buf
        id: CAN id
        secs: Seconds, needed for Socketcand frame, but not apart of traditional CAN frame.
            Time can be referenced from any point depending on users needs.
        usecs: Microseconds, same details as seconds.
        data[8]: CAN data
        len: length of data  
    RETURNS:
        r: number of characters written to the buffer, can be used to find errors
*/
int socketcand_translate_frame_to_string(char * buf, int bufsize, uint32_t id, unsigned long secs, 
                                         unsigned long usecs, uint8_t data[8], int len, uint8_t ext) {
    int r, i;    
    if (len>8) {
        return 0;
    }
    // Socketcand translate makes all frames extended by default
    if(ext) {
      r = snprintf(buf, bufsize-r, "< frame %08X %ld.%06ld ", id & SOCKETCAND_TRANSLATE_EXT_MASK, secs, usecs);
    } else {
      r = snprintf(buf, bufsize-r, "< frame %08X %ld.%06ld ", id & SOCKETCAND_TRANSLATE_STD_MASK, secs, usecs);
    }
    //loop needed to properly parse data by byte
    for(i = 0; i < len; i++) {
        r += snprintf(buf + r, bufsize-r, "%02X", data[i]);
    }
    snprintf(buf + r, bufsize-r, " >");
    socketcand_translate_string_out(buf);
    return r;
    
}


/*  Used to parse CAN data out of Socketcand send frames and organize in a struct
    PARAMETERS:
        buf: Socketcand send frame: < send [id] [length] [data] >
        f: frame to store CAN data in
    RETURNS:
        f: frame with CAN data if no errors, if errors will return frame with err flag
*/
struct socketcand_translate_frame socketcand_translate_string_to_frame(char * buf, struct socketcand_translate_frame f) {
    int count = 0;
    if(strncmp("< send ", buf, 7) == 0) {
      count = sscanf(
          buf, "< %*s %x %hhu %hhx %hhx %hhx %hhx %hhx %hhx %hhx %hhx >",
          &f.id, &f.len, &f.data[0], &f.data[1], &f.data[2], &f.data[3], 
          &f.data[4],&f.data[5], &f.data[6], &f.data[7]);

      if((count < 2) || (f.len > 8) || (count != 2 + f.len)) {
        f.id |= SOCKETCAND_TRANSLATE_ERR_FLAG;
      }
      if(id_len(buf) == 8) {
        f.ext = 1;
      }
    } else {
      f.id |= SOCKETCAND_TRANSLATE_ERR_FLAG;
    }
    return f;
}


/*  This function is used to get the length of a CAN id from a < send > frame
    PARAMETERS:
        buf: buffer with < send > frame
    RETURNS:
        len: length of CAN id    
*/
int id_len(char *buf) {
  int len = 0;
  char *elembuf;

  elembuf = &buf[7]; //if valid send command id will always start at 7th index
  if(elembuf == NULL) {
    return 0;
  }
  while(elembuf[len] != '\0' && elembuf[len] != ' ') {
    len++;
  }
  return len;
}


/*  This function is used to mimic the socketcand protocol for opening a rawmode.
    PARAMETERS:
        round: When opening rawmode you are sent 3 messages and have to reply to 
        the first 2, round corrosponds to which stage of opening rawmode you are on
        buf: Carries message from user and is then filled with response message
        bufsize: size of buf
    RETURNS:
        The new round number, or -1 if buf is too small for this application
*/
int socketcand_translate_open_raw(int round, char * buf, int bufsize) {
    if (bufsize < 12) {
        return 0;
    } else 
    if (round == 0) {
        snprintf(buf, bufsize, "< hi >");
        return 1;
    } else
    if (round == 1) {
        if (strncmp("< open ", buf, 7) == 0) {
            snprintf(buf, bufsize, "< ok >");
            return 2;
        }
    } else
    if (round == 2) {
        if (strncmp("< rawmode >", buf, 11) == 0) {
            snprintf(buf, bufsize, "< ok >");
            socketcand_translate_state = SOCKETCAND_TRANSLATE_OPEN;
            return 2;
        }
    }
    snprintf(buf, bufsize, "< error >");
    socketcand_translate_string_out(buf);
    return 0;
}


/*  Returns whether the daemon is open or not, this is dependent on the conditions
    of socketcand rawmode being activated.
    RETURNS:
        socketcand_translate_state: Open (1) or closed (0)
*/
int socketcand_translate_is_open() {
    return socketcand_translate_state;
}

int socketcand_translate_set_state(int state) {
    socketcand_translate_state = state;
}

// To be implimented per user needs
int socketcand_translate_string_out(char * s) {}


// To be implimented per users needs
int socketcand_translate_struct_out(struct socketcand_translate_frame f) {}

