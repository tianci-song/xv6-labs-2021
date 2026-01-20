struct buf {
  int valid;   // has data been read from disk?
  int disk;    // does disk "own" buf?
  uint dev;
  uint blockno;
  struct sleeplock lock;
  uint refcnt;
	uint ticks;	// record which buf is the least recently used
  uchar data[BSIZE];
};

