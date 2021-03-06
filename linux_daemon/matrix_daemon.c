/*
 * Copyright (C) 2012 Phillip Pearson <pp@myelin.co.nz>
 */

#include "matrix_daemon.h"

int portfd;
int debuglevel = 1;

int setup_serial(const char* port_path) {
  int fd;
  struct termios options;
#ifdef TRICKY_FAST_RS232
  struct serial_struct serialinfo;
#endif

  printf("Opening serial port %s\n", port_path);
  fd = open(port_path, O_RDWR | O_NOCTTY | O_NDELAY);
  if (fd == -1) {
    perror("open_port: Unable to open port - ");
    exit(1);
  }
  printf("opened; fd %d\n", fd);

  /* set blocking -- fcntl(fd, F_SETFL, FNDELAY); to make read() return 0 if no chars are available */
  fcntl(fd, F_SETFL, 0);

  printf("- Setting termios options\n");
  tcgetattr(fd, &options);
#ifdef EASY_FAST_RS232
#define BASE_BAUD B1000000
#else
#ifdef TRICKY_FAST_RS232
#define BASE_BAUD B38400
#else
#define BASE_BAUD B115200
#endif
#endif
  if (cfsetispeed(&options, BASE_BAUD)) {
    perror("failed to set port to 115200 bps");
    exit(1);
  }
  if (cfsetospeed(&options, BASE_BAUD)) {
    perror("failed to set port output speed to 115200 bps");
    exit(1);
  }
  options.c_cflag |= (CLOCAL | CREAD);
  // 8N1
  //  options.c_cflag &= ~PARENB;
  //  options.c_cflag &= ~CSTOPB;
  //  options.c_cflag &= ~CSIZE;
  //  options.c_cflag |= CS8;
  //  options.c_cflag |= CRTSCTS; // hardware flow control
  //  options.c_iflag &= ~(IXON | IXOFF | IXANY); // no software flow control
  //  options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG); // raw input
  //  options.c_oflag &= ~OPOST; // raw output

  // from http://todbot.com/arduino/host/arduino-serial/arduino-serial.c
  // 8N1
  options.c_cflag &= ~PARENB;
  options.c_cflag &= ~CSTOPB;
  options.c_cflag &= ~CSIZE;
  options.c_cflag |= CS8;
  // no flow control
  options.c_cflag &= ~CRTSCTS;

  options.c_cflag |= CREAD | HUPCL | CLOCAL;  // turn on READ & ignore ctrl lines
  options.c_iflag = 0; // &= ~(IXON | IXOFF | IXANY); // turn off s/w flow ctrl

  options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG); // make raw
  options.c_oflag = 0; // &= ~OPOST; // make raw

  // see: http://unixwiz.net/techtips/termios-vmin-vtime.html
  options.c_cc[VMIN]  =
    /*
#ifdef __linux__
    255
#else
    1
#endif
    */
    0 // all reads are nonblocking
    ;
  options.c_cc[VTIME] = 5; // 0.5 s

  // set termios options
  tcsetattr(fd, TCSANOW, &options);

#ifdef TRICKY_FAST_RS232
  printf("- Setting Linux serial_struct options\n");
  ioctl(fd, TIOCGSERIAL, &serialinfo);
  serialinfo.custom_divisor = serialinfo.baud_base / BAUD;
  serialinfo.flags = (serialinfo.flags & ~ASYNC_SPD_MASK) | ASYNC_SPD_CUST;
  ioctl(fd, TIOCSSERIAL, &serialinfo);
  printf("- Custom divisor set to %d (baud_base is %d), which should give us %d baud\n",
	 (int)serialinfo.custom_divisor, (int)serialinfo.baud_base,
	 (int)(serialinfo.baud_base / serialinfo.custom_divisor));
#endif

  printf("- Serial port open\n");
  return fd;
}

int millitime(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec * 1000 + (tv.tv_usec / 1000);
}

struct Frame {
  int w, h;
  size_t buffer_len;
  unsigned char *buffer;
  Frame(int _w, int _h) {
    w = _w;
    h = _h;
    buffer_len = w * h * 3;
    buffer = (unsigned char*)malloc(buffer_len);
    if (!buffer) {
      printf("unable to allocate %d bytes for frame buffer\n", (int)buffer_len);
      exit(1);
    }
  }
  // clear screen
  void blank() {
    bzero(buffer, buffer_len);
  }
  int pos_from_xy(int x, int y) {
    return y * WIDTH + (y & 1 ? (24 - x) : x);
  }
  // set entire frame at once, from a buffer in column format that isn't aware of the pixel layout
  void set_from_buffer(const unsigned char *buf) {
    int i = 0;
    for (int y = 0; y < HEIGHT; ++y) {
      for (int x = 0; x < WIDTH; ++x) {
	unsigned char* ptr = buffer + pos_from_xy(x, y) * 3;
	*ptr++ = buf[i++];
	*ptr++ = buf[i++];
	*ptr++ = buf[i++];
      }
    }
  }
  void set(int x, int y, unsigned char r, unsigned char g, unsigned char b) {
    int pos = pos_from_xy(x, y);
    buffer[pos * 3] = r;
    buffer[pos * 3 + 1] = g;
    buffer[pos * 3 + 2] = b;
  }
  void rect(int x0, int y0, int x1, int y1, unsigned char r, unsigned char g, unsigned char b) {
    for (int y = y0; y < y1; ++y) {
      for (int x = x0; x < x1; ++x) {
	set(x, y, r, g, b);
      }
    }
  }
};

// set port blocking (or not)
void set_blocking(int blocking) {
  fcntl(portfd, F_SETFL, blocking ? 0 : FNDELAY);
}

int main(int argc, char *argv[]) {
  int buf_size = 1024;
  unsigned char buf[buf_size];
  unsigned int total_bytes_sent = 0;
  Frame frame(WIDTH, HEIGHT);
  int n_frame;
  int start_millis = millitime();
  int last_frame_start_millis = start_millis;
  std::deque<unsigned char> input;
  const char* preamble = "OK.";

  int in_udp_mode = 0;

  printf("Want It! matrix display (C) 2012 Phillip Pearson, pp@myelin.co.nz\n");

  if (argc < 2) {
    printf("Syntax: %s </path/to/serial/port>\n", argv[0]);
    exit(1);
  }

  portfd = setup_serial(argv[1]);
  setup_udp();

  printf("Flushing serial input in case we have a startup announcement buffered (if this is an Uno)\n");
  while (read(portfd, buf, 1) > 0); // VMIN=0, VTIME=5 means this will terminate when we don't get a byte in 0.5 s

  //test_loop_corruption();

  for (n_frame = 0; ; ++n_frame) {
    int frame_start_millis = millitime();
    int ready_for_next = 0;
    size_t preamble_read_pos = 0;
    int ms_since_start = frame_start_millis - start_millis;
    int bytes_per_sec = ms_since_start ? (int)((double)total_bytes_sent * 1000 / ms_since_start) : 0;
    int ms_per_frame = frame_start_millis - last_frame_start_millis;
    if (debuglevel >= 1) {
      printf("%d ms since last frame / %d bytes in %d ms / %d bytes/sec / %.1f fps\n",
	     ms_per_frame,
	     total_bytes_sent,
	     ms_since_start,
	     bytes_per_sec,
	     1000.0 / ms_per_frame);
    }
    last_frame_start_millis = frame_start_millis;

    while (!ready_for_next) {
      ssize_t bytes_read = read(portfd, buf, 1);
      if (bytes_read == 0) {
	printf("write junk data to kick the avr\n");
	if (write(portfd, ".", 1) == -1) {
	  printf("disconnect!\n");
	  exit(2);
	}
	usleep(1000);
	continue;
      }

      if (bytes_read == -1) {
	perror("error reading from serial port");
	continue;
      }

      if (debuglevel >= 10) printf("%d: ", (int)bytes_read);
      for (ssize_t i = 0; i < bytes_read; ++i) {
	input.push_back(buf[i]);
	if (debuglevel >= 10) {
	  if (buf[i] < 32) printf("<%02x>", buf[i]);
	  else putchar(buf[i]);
	}
      }
      if (debuglevel >= 10) printf("\n");

      while (input.size()) {
	if (preamble_read_pos == strlen(preamble)) {
	  ready_for_next = 1;
	  break;
	}
	unsigned char c = input.front();
	input.pop_front();
	if (c == preamble[preamble_read_pos]) ++preamble_read_pos;
      }
    }

    if (n_frame == 0) {
      // arduino just initialized; reset timers
      last_frame_start_millis = frame_start_millis = start_millis = millitime();
    }

    if (debuglevel >= 2) printf("[waited %d milliseconds]\n", millitime() - frame_start_millis);

    // do something
    frame.blank();
    //frame.rect(0, 0, WIDTH, HEIGHT, rand() % 256, rand() % 256, rand() % 256);
    frame.set(n_frame % LEN, 0, 0xff, 0xff, 0xff); //rand() & 0xff, rand() & 0xff, rand() & 0xff);

    // see if someone sent us something over UDP (Disorient protocol / Processing interface)
    unsigned char udp_buffer[frame.buffer_len];
    int received_udp_frame = check_udp(udp_buffer, frame.buffer_len);
    if (received_udp_frame) {
      // received a udp frame -- disable internal pattern generation for now
      in_udp_mode = 1;
    } else if (in_udp_mode && !received_udp_frame) {
      // expecting a udp frame - wait for up to a second, and if we don't get anything, drop back into internal mode
      if (debuglevel > 5) printf("waiting for a bit to see if we'll get a udp frame\n");
      in_udp_mode = 0;
      for (int udp_try = 0; udp_try < 1000; ++udp_try) {
	usleep(1000); // 1 ms
	received_udp_frame = check_udp(udp_buffer, frame.buffer_len);
	if (received_udp_frame) {
	  in_udp_mode = 1;
	  break;
	}
      }
    }
    if (received_udp_frame) frame.set_from_buffer(udp_buffer);

    // clear out any excess udp frames
    while (check_udp(NULL, 0)) printf("skipped too-fast UDP frame\n");

    if (debuglevel >= 1) printf("Sending frame %d%s\n", n_frame, in_udp_mode ? " (UDP)" : "");
    //set_blocking(1);
    buf[0] = buf[1] = '*';
    buf[2] = buf[3] = '+';
    if (write(portfd, buf, 4) != 4) {
      printf("failed to write packet start\n");
    }
    //set_blocking(0);
    while (1) {
      unsigned char c;
      while (read(portfd, &c, 1) < 1) {
	if (debuglevel >= 10) printf("nothing\n");
	if (write(portfd, "", 0) == -1) {
	  printf("disconnect!\n");
	  exit(2);
	}
	usleep(10000);
      }
      if (c == '#') {
	if (debuglevel >= 5) printf("received start signal from avr\n");
	break;
      } else if (debuglevel >= 10) {
	if (c < 32)
	  printf("received junk byte <%02x> from avr\n", (int)c);
	else
	  printf("received junk byte '%c' from avr\n", (int)c);
      }
    }
    set_blocking(1);
    ssize_t bytes_written = write(portfd, frame.buffer, 900);
    total_bytes_sent += bytes_written;
    if (debuglevel >= 5) printf("(total %d bytes)\n", (int)bytes_written);
    if (bytes_written != LEN * 3) {
      printf("not 900?\n");
      exit(1);
    }
  }

  close(portfd);

  return 0;
}
