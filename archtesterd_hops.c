
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <netinet/if_ether.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <errno.h>

//
// Constants for the protocols formats
//

#define ARCHTESTERD_IP4_HDRLEN			20
#define ARCHTESTERD_ICMP4_HDRLEN		 8
#define ARCHTESTERD_ICMP_ECHOREPLY		 0
#define ARCHTESTERD_ICMP_DEST_UNREACH		 3
#define ARCHTESTERD_ICMP_ECHO			 8
#define ARCHTESTERD_ICMP_TIME_EXCEEDED		11


//
// Types
//

typedef uint16_t archtesterd_idtype;

enum archtesterd_algorithms {
  archtesterd_algorithms_random,
  archtesterd_algorithms_sequential,
  archtesterd_algorithms_reversesequential,
  archtesterd_algorithms_binarysearch
};

enum archtesterd_responseType {
  archtesterd_responseType_echoResponse,
  archtesterd_responseType_destinationUnreachable,
  archtesterd_responseType_timeExceeded
};

struct archtesterd_probe {
  int used;
  archtesterd_idtype id;
  unsigned char hops;
  unsigned int probeLength;
  struct timeval sentTime;
  int responded;
  unsigned int responseLength;
  struct timeval responseTime;
  unsigned long delayUSecs;
  enum archtesterd_responseType responseType;
};

//
// Constants
//

#define archtesterd_algorithms_string	"random, sequential, reversesequential, or binarysearch"

#define ARCHTESTERD_MAX_PROBES	       256

//
// Variables
//

const char* testDestination = "www.google.com";
struct sockaddr_in testDestinationAddress;
const char* interface = "eth0";
static int debug = 0;
static int progress = 1;
static int progressDetailed = 0;
static int conclusion = 1;
static int statistics = 0;
static unsigned int startTtl = 1;
static unsigned int maxTtl = 255;
static unsigned int maxProbes = 50;
static unsigned int parallel = 1;
static unsigned int bucket = 0;
static unsigned int icmpDataLength = 0;
static enum archtesterd_algorithms algorithm = archtesterd_algorithms_sequential;
static struct archtesterd_probe probes[ARCHTESTERD_MAX_PROBES];
static unsigned int probesSent = 0;
static unsigned char currentTtl = 0;
static int hopsMin = -1;
static int hopsMax = 255;

//
// Prototype definitions of functions
//

static void
archtesterd_reportBriefConclusion(void);

//
// Some helper macros
//

#define archtesterd_min(a,b) ((a) < (b) ? (a) : (b))
#define archtesterd_max(a,b) ((a) > (b) ? (a) : (b))

//
// Debug helper function
//

static void
debugf(const char* format, ...) {
  
  if (debug) {

    va_list args;

    printf("archtesterd_hops: debug: ");
    va_start (args, format);
    vprintf(format, args);
    va_end (args);
    printf("\n");
    
  }
  
}

//
// Display a fatal error
//

static void
fatalf(const char* format, ...) {
  
  va_list args;
  
  fprintf(stderr,"archtesterd_hops: error: ");
  va_start (args, format);
  vfprintf(stderr, format, args);
  va_end (args);
  fprintf(stderr," -- exit\n");
  
  exit(1);
}

//
// Display a fatal error a la perror
//

static void
fatalp(const char* message) {
  
  const char* string = strerror(errno);
  fatalf("system: %s", string);
  
}

//
// String processing: fill a buffer with string (cut/repeated as needed
// to fill the expected length)
//

static void
archtesterd_fillwithstring(char* buffer,
			   const char* string,
			   unsigned char bufferSize) {
  
  const char* stringPointer = string;
  while (bufferSize > 0) {
    *buffer = *stringPointer;
    buffer++;
    bufferSize--;
    if (*stringPointer != '\0') stringPointer++;
    if (*stringPointer == '\0') stringPointer = string;
  }
  
}

static unsigned long
archtesterd_timediffinusecs(struct timeval* later,
			    struct timeval* earlier) {
  if (later->tv_sec < earlier->tv_sec) {
    fatalf("expected later time to be greater, second go back %uls", earlier->tv_sec - later->tv_sec);
  }
  if (later->tv_sec == earlier->tv_sec) {
    if (later->tv_usec < earlier->tv_usec) {
      fatalf("expected later time to be greater, microsecond go back %uls", earlier->tv_usec - later->tv_usec);
    }
    return(later->tv_usec - earlier->tv_usec);
  } else {
    unsigned long result = 1000 * 1000 * (later->tv_sec - earlier->tv_sec);
    result += (1000*1000) - earlier->tv_usec;
    result += later->tv_usec;
    return(result);
  }
}

//
// Convert an IPv4 address to string
//

const char*
archtesterd_addrtostring(struct in_addr* addr) {
  return(inet_ntoa(*addr));
}

//
// Add a new probe entry
//

struct archtesterd_probe*
archtesterd_newprobe(archtesterd_idtype id,
		     unsigned char hops,
		     unsigned int probeLength) {
  
  struct archtesterd_probe* probe = &probes[id];
  if (probe->used) {
    fatalf("cannot allocate a new probe for id %u", (unsigned int)id);
    return(0);
  }

  memset(probe,0,sizeof(*probe));
  
  probe->used = 1;
  probe->id = id;
  probe->hops = hops;
  probe->probeLength = probeLength;
  probe->responded = 0;
  
  if (gettimeofday(&probe->sentTime, 0) < 0) {
    fatalp("cannot determine current time via gettimeofday");
  }
  
  debugf("registered a probe for id %u, ttl %u", id, hops);
  
  probesSent++;
  if (bucket > 0) bucket--;
  return(probe);
}

struct archtesterd_probe*
archtesterd_findprobe(archtesterd_idtype id) {
  
  struct archtesterd_probe* probe = &probes[id];
  
  if (!probe->used) {
    debugf("have not sent a probe with id %u", (unsigned int)id);
    return(0);
  } else if (probe->responded) {
    debugf("have already seen a response to the probe with id %u", (unsigned int)id);
    return(0);
  } else {
    return(probe);
  }
  
}

static void
archtesterd_registerResponse(enum archtesterd_responseType type,
			     archtesterd_idtype id,
			     unsigned int packetLength,
			     struct archtesterd_probe** responseToProbe) {

  //
  // See if we can find the probe that this is a response to
  //
  
  struct archtesterd_probe* probe = archtesterd_findprobe(id);
  if (probe == 0) {
    debugf("cannot find the probe that response id %u was a response to", id);
    *responseToProbe = 0;
    return;
  }

  //
  // Look at the state of the probe
  //

  if (probe->responded) {
    debugf("we have already seen a response to probe id %u", id);
    *responseToProbe = probe;
    return;
  }
  
  //
  // This is new. Update the probe data
  //

  debugf("this is a new valid response to probe id %u", id);
  probe->responded = 1;
  probe->responseLength = packetLength;
  if (gettimeofday(&probe->responseTime, 0) < 0) {
    fatalp("cannot determine current time via gettimeofday");
  }
  probe->delayUSecs = archtesterd_timediffinusecs(&probe->responseTime,
						  &probe->sentTime);
  debugf("probe delay was %.3f ms", probe->delayUSecs / 1000.0);
  probe->responseType = type;

  //
  // Update our conclusions about the destination
  //
  
  if (type == archtesterd_responseType_echoResponse) {
    hopsMax = archtesterd_min(hopsMax,probe->hops);
    debugf("echo reply means hops is at most %u", hopsMax);
  }
  if (type == archtesterd_responseType_timeExceeded) {
    hopsMin = archtesterd_max(hopsMin,probe->hops + 1);
    debugf("time exceeded means hops is at least %u", hopsMin);
  }

  //
  // Update the task counters
  //
  
  bucket++;
  if (bucket > parallel) bucket = parallel;
  
  //
  // Return, and set output parameters
  //
  
  *responseToProbe = probe;
}

//
// Allocate new identifiers for the different probes
//

archtesterd_idtype
archtesterd_getnewid(unsigned char hops) {
  
  static unsigned int nextId = 0;
  unsigned int id;
  
  do {
    
    id = nextId++;
    struct archtesterd_probe* probe = &probes[id];
    if (probe->used) continue;
    else return(id);
    
  } while (id <= 65535 && id +1 < ARCHTESTERD_MAX_PROBES);
  
  fatalf("cannot find a new identifier for %u hops", hops);
  return(0);
}

//
// Finding out an interface index for a named interface
//

static void
archtesterd_getifindex(const char* interface,
		       int* ifIndex,
		       struct ifreq* ifrp,
		       struct sockaddr_in *addr) {
  
  struct ifreq ifr;
  int sd;
  
  //
  // Get a raw socket
  //
  
  if ((sd = socket (AF_INET, SOCK_RAW, IPPROTO_RAW)) < 0) {
    fatalp("socket() failed to get socket descriptor for using ioctl()");
  }
  
  //
  // Find interface index
  //
  
  memset (&ifr,0,sizeof (ifr));
  strncpy(ifr.ifr_name, interface, sizeof (ifr.ifr_name));
  if (ioctl (sd, SIOCGIFINDEX, &ifr) < 0) {
    fatalp("ioctl() failed to find interface");
  }
  *ifIndex = ifr.ifr_ifindex;
  *ifrp = ifr;
  
  //
  // Find own source address
  //
  
  memset (&ifr,0,sizeof (ifr));
  strncpy(ifr.ifr_name, interface, sizeof (ifr.ifr_name));
  if (ioctl(sd,SIOCGIFADDR,&ifr)==-1) {
    fatalp("ioctl() failed to find interface address");
  }
  *addr = *(struct sockaddr_in*)&ifr.ifr_addr;
  
  //
  // Cleanup and return
  //

  close (sd);
}

static void
archtesterd_getdestinationaddress(const char* destination,
				  struct sockaddr_in* address) {
  
  struct addrinfo hints, *res;
  struct sockaddr_in *addr;
  int rcode;
  
  memset(&hints,0,sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = hints.ai_flags | AI_CANONNAME;
  
  if ((rcode = getaddrinfo(destination, NULL, &hints, &res)) != 0) {
    fprintf (stderr, "archtesterd_hops: cannot resolve address %s: %s\n", destination, gai_strerror (rcode));
    exit(1);
  }
  *address = *(struct sockaddr_in*)res->ai_addr;
}

//
// Mapping addresses to strings (n.n.n.n or h:h:...:h)
//

const char*
archtesterd_iptostring(struct sockaddr_in* in) {
  char* result = (char*)malloc(INET_ADDRSTRLEN+1);
  memset(result,0,INET_ADDRSTRLEN+1);
  if (inet_ntop (AF_INET, (void*)&in->sin_addr, result, INET_ADDRSTRLEN) == NULL) {
    fatalf("inet_ntop() failed");
  }
}

//
// Checksum per RFC 1071
//

uint16_t
archtesterd_checksum(uint16_t* data,
		     int length)
{
  register uint32_t sum = 0;
  int count = length;
  
  while (count > 1) {
    sum += *(data++);
    count -= 2;
  }
  
  if (count > 0) {
    sum += *(uint8_t*)data;
  }
  
  while (sum >> 16) {
    sum = (sum & 0xffff) + (sum >> 16);
  }
  
  return(~sum);
}

//
// Construct an ICMPv4 packet
//

static void
archtesterd_constructicmp4packet(struct sockaddr_in* source,
				 struct sockaddr_in* destination,
				 archtesterd_idtype id,
				 unsigned char ttl,
				 unsigned int dataLength,
				 char** resultPacket,
				 unsigned int* resultPacketLength) {

  static const char* message = "archtester";
  static char data[IP_MAXPACKET];
  static char packet[IP_MAXPACKET];
  struct icmp icmphdr;
  struct ip iphdr;
  unsigned int icmpLength;
  unsigned int packetLength;

  //
  // Make some checks
  //

  if (ARCHTESTERD_IP4_HDRLEN + ARCHTESTERD_ICMP4_HDRLEN + dataLength > IP_MAXPACKET) {
    fatalf("requesting to make a too long IP packet for data length %u", dataLength);
  }
  
  //
  // Fill in ICMP parts
  //
  
  icmphdr.icmp_type = ARCHTESTERD_ICMP_ECHO;
  icmphdr.icmp_code = 0;
  icmphdr.icmp_id = id;
  icmphdr.icmp_seq = (uint16_t)(probesSent & 0xFFFF);
  icmphdr.icmp_cksum = 0;
  archtesterd_fillwithstring(data,message,dataLength);
  icmpLength = ARCHTESTERD_ICMP4_HDRLEN + dataLength;
  memcpy(packet + ARCHTESTERD_IP4_HDRLEN,&icmphdr,ARCHTESTERD_ICMP4_HDRLEN);
  memcpy(packet + ARCHTESTERD_IP4_HDRLEN + ARCHTESTERD_ICMP4_HDRLEN,data,dataLength);
  icmphdr.icmp_cksum = archtesterd_checksum((uint16_t*)(packet + ARCHTESTERD_IP4_HDRLEN), icmpLength);
  memcpy(packet + ARCHTESTERD_IP4_HDRLEN,&icmphdr,ARCHTESTERD_ICMP4_HDRLEN);
  
  //
  // Fill in the IPv4 header
  //
  
  iphdr.ip_hl = 5;
  iphdr.ip_v = 4;
  iphdr.ip_tos = 0;
  packetLength = ARCHTESTERD_IP4_HDRLEN + icmpLength;
  iphdr.ip_len = htons (packetLength);
  iphdr.ip_id = id;
  iphdr.ip_off = 0;
  iphdr.ip_ttl = ttl;
  iphdr.ip_p = IPPROTO_ICMP;
  iphdr.ip_src = source->sin_addr;
  iphdr.ip_dst = destination->sin_addr;
  iphdr.ip_sum = 0;
  iphdr.ip_sum = archtesterd_checksum((uint16_t*)&iphdr,ARCHTESTERD_IP4_HDRLEN);
  memcpy (packet, &iphdr, ARCHTESTERD_IP4_HDRLEN);

  //
  // Debugs
  //
  
  debugf("constructed a packet of %u bytes, ttl = %u", packetLength, ttl);
  
  //
  // Return the packet
  //
  
  *resultPacket = packet;
  *resultPacketLength = packetLength;
}

//
// Send a plain IP packet to raw socket
//

static void
archtesterd_sendpacket(int sd,
		       char* packet,
		       unsigned int packetLength,
		       struct sockaddr* addr,
		       size_t addrLength)  {
  
  if (sendto (sd, packet, packetLength, 0, addr, sizeof (struct sockaddr)) < 0) {
    fatalp("sendto() failed");
  }
  
}

//
// Receive a packet from the raw socket
//

static int
archtesterd_receivepacket(int sd,
			  char** result) {
  
  static char packet[IP_MAXPACKET];
  struct sockaddr from;
  socklen_t fromlen;
  int bytes;
  
  debugf("waiting for responses");
  
  if ((bytes = recvfrom (sd,
			 packet,
			 sizeof(packet),
			 MSG_DONTWAIT,
			 (struct sockaddr *) &from,
			 &fromlen)) < 0) {
    fatalp("socket() failed to read from the raw socket");
  }
  
  *result = packet;
  return(bytes);
}

//
// IP & ICMP packet validation
//

static int
archtesterd_validatepacket(char* receivedPacket,
			   int receivedPacketLength,
			   enum archtesterd_responseType* responseType,
			   archtesterd_idtype* responseId,
			   struct ip* responseToIpHdr,
			   struct icmp* responseToIcmpHdr) {
  
  struct ip iphdr;
  struct icmp icmphdr;

  //
  // Validate IP4 header
  //
  
  if (receivedPacketLength < ARCHTESTERD_IP4_HDRLEN) return(0);
  memcpy(&iphdr,receivedPacket,ARCHTESTERD_IP4_HDRLEN);
  if (iphdr.ip_v != 4) return(0);
  if (ntohs(iphdr.ip_len) < receivedPacketLength) return(0);
  if (iphdr.ip_off != 0) return(0);
  if (iphdr.ip_p != IPPROTO_ICMP) return(0);
  // TODO: check iphdr.ip_sum ...
  
  //
  // Validate ICMP4 header
  //
  
  if (receivedPacketLength < ARCHTESTERD_IP4_HDRLEN + ARCHTESTERD_ICMP4_HDRLEN) return(0);
  memcpy(&icmphdr,&receivedPacket[ARCHTESTERD_IP4_HDRLEN],ARCHTESTERD_ICMP4_HDRLEN);
  *responseId = icmphdr.icmp_id;
  
  // TODO: check icmphdr.icmp_sum ...

  switch (icmphdr.icmp_type) {

  case ARCHTESTERD_ICMP_ECHOREPLY:
    *responseType = archtesterd_responseType_echoResponse;
    debugf("ECHO RESPONSE from %s", archtesterd_addrtostring(&iphdr.ip_src));
    break;

  case ARCHTESTERD_ICMP_TIME_EXCEEDED:
    if (icmphdr.icmp_code != 0) {
      debugf("ICMP code in TIME EXCEEDED is not 0");
      return(0);
    }
    if (ntohs(iphdr.ip_len) <
	ARCHTESTERD_IP4_HDRLEN + ARCHTESTERD_ICMP4_HDRLEN +
	ARCHTESTERD_IP4_HDRLEN + ARCHTESTERD_ICMP4_HDRLEN) {
      debugf("ICMP error does not include enough of the original packet", ntohs(iphdr.ip_len));
      return(0);
    }
    memcpy(responseToIpHdr,
	   &receivedPacket[ARCHTESTERD_IP4_HDRLEN+ARCHTESTERD_ICMP4_HDRLEN],
	   ARCHTESTERD_IP4_HDRLEN);
    memcpy(responseToIcmpHdr,
	   &receivedPacket[ARCHTESTERD_IP4_HDRLEN+ARCHTESTERD_ICMP4_HDRLEN+ARCHTESTERD_IP4_HDRLEN],
	   ARCHTESTERD_ICMP4_HDRLEN);
    *responseId = responseToIcmpHdr->icmp_id;
    debugf("inner header as seen by archtesterd_validatepacket:");
    debugf("  inner ip proto = %u", responseToIpHdr->ip_p);
    debugf("  inner ip len = %u", ntohs(responseToIpHdr->ip_len));
    debugf("  inner ip id = %u", responseToIpHdr->ip_id);
    debugf("  inner ip offset = %u", responseToIpHdr->ip_off);
    debugf("  inner icmp type = %u", responseToIcmpHdr->icmp_type);
    debugf("  inner icmp code = %u", responseToIcmpHdr->icmp_code);
    debugf("using inner id %u in ICMP error", *responseId);
    if (responseToIpHdr->ip_p != IPPROTO_ICMP &&
	responseToIcmpHdr->icmp_type != ARCHTESTERD_ICMP_ECHO) {
      debugf("ICMP error includes some other packet than ICMP ECHO proto = %u icmp code = %u",
	     responseToIpHdr->ip_p,
	     responseToIcmpHdr->icmp_type);
      return(0);
    }
    *responseType = archtesterd_responseType_timeExceeded;
    debugf("TIME EXCEEDED from %s", archtesterd_addrtostring(&iphdr.ip_src));
    break;

  case ARCHTESTERD_ICMP_DEST_UNREACH:
    if (ntohs(iphdr.ip_len) <
	ARCHTESTERD_IP4_HDRLEN + ARCHTESTERD_ICMP4_HDRLEN +
	ARCHTESTERD_IP4_HDRLEN + ARCHTESTERD_ICMP4_HDRLEN) {
      debugf("ICMP error does not include enough of the original packet", ntohs(iphdr.ip_len));
      return(0);
    }
    memcpy(responseToIpHdr,
	   &receivedPacket[ARCHTESTERD_IP4_HDRLEN+ARCHTESTERD_ICMP4_HDRLEN],
	   ARCHTESTERD_IP4_HDRLEN);
    memcpy(responseToIcmpHdr,
	   &receivedPacket[ARCHTESTERD_IP4_HDRLEN+ARCHTESTERD_ICMP4_HDRLEN+ARCHTESTERD_IP4_HDRLEN],
	   ARCHTESTERD_ICMP4_HDRLEN);
    *responseId = responseToIcmpHdr->icmp_id;
    debugf("using inner id %u in ICMP error", *responseId);
    if (responseToIpHdr->ip_p != IPPROTO_ICMP &&
	responseToIcmpHdr->icmp_type != ARCHTESTERD_ICMP_ECHO) {
      debugf("ICMP error includes some other packet than ICMP ECHO proto = %u icmp code = %u",
	     responseToIpHdr->ip_p,
	     responseToIcmpHdr->icmp_type);
      return(0);
    }
    *responseType = archtesterd_responseType_destinationUnreachable;
    debugf("DESTINATION UNREACHABLE from %s", archtesterd_addrtostring(&iphdr.ip_src));
    break;
    
  default:
    return(0);
    
  }
  
  //
  // Seems OK
  //
  
  return(1);
}

//
// Check that the packet is for this node and tht it is an
// ICMP packet
//

static int
archtesterd_packetisforus(char* receivedPacket,
			  int receivedPacketLength,
			  enum archtesterd_responseType receivedResponseType,
			  struct sockaddr_in* sourceAddress,
			  struct sockaddr_in* destinationAddress,
			  struct ip* responseToIpHdr,
			  struct icmp* responseToIcmpHdr) {
  
  struct ip iphdr;

  //
  // Check the destination is our source address
  //
  
  memcpy(&iphdr,receivedPacket,ARCHTESTERD_IP4_HDRLEN);
  if (memcmp(&iphdr.ip_dst,&sourceAddress->sin_addr,sizeof(iphdr.ip_dst)) != 0) return(0);
  
  //
  // For ICMP error messages, we've already checked that the included
  // packet was an ICMP ECHO.  Now we need to additionally verify that
  // it was one sent from us to the destination.
  //
  
  if (receivedResponseType == archtesterd_responseType_destinationUnreachable ||
      receivedResponseType == archtesterd_responseType_timeExceeded) {
    
    debugf("checking that inner packet in the ICMP error was sent by us");
    debugf("  inner ip proto = %u", responseToIpHdr->ip_p);
    debugf("  inner ip len = %u", ntohs(responseToIpHdr->ip_len));
    debugf("  inner ip id = %u", responseToIpHdr->ip_id);
    debugf("  inner ip offset = %u", responseToIpHdr->ip_off);
    debugf("  inner icmp type = %u", responseToIcmpHdr->icmp_type);
    debugf("  inner icmp code = %u", responseToIcmpHdr->icmp_code);
    if (memcmp(&responseToIpHdr->ip_src,&sourceAddress->sin_addr,sizeof(responseToIpHdr->ip_src)) != 0) return(0);
    debugf("checking that inner packet in the ICMP error was sent to the destination we are testing");
    if (memcmp(&responseToIpHdr->ip_dst,&destinationAddress->sin_addr,sizeof(responseToIpHdr->ip_dst)) != 0) return(0);
    debugf("inner packet checks ok");
    
  }
  
  //
  // Looks like it is for us
  //
  
  return(1);
}

//
// Reporting progress: sent
//

static void
archtesterd_reportprogress_sent(archtesterd_idtype id,
				unsigned char ttl) {
  if (progress) {
    printf("ECHO #%u (TTL %u)...", id, ttl);
  }
}

//
// Reporting progress: sending many in parallel
//

static void
archtesterd_reportprogress_sendingmore() {
  if (progress) {
    printf("\n");
  }
}

//
// Reporting progress: received
//

static void
archtesterd_reportprogress_received(enum archtesterd_responseType responseType,
				    archtesterd_idtype id,
				    unsigned char ttl) {
  
  if (progress) {
    switch (responseType) {
    case archtesterd_responseType_echoResponse:
      printf(" <--- #%u REPLY", id);
      if (progressDetailed) {
	printf(": ");
	archtesterd_reportBriefConclusion();
      }
      printf("\n");
      break;
    case archtesterd_responseType_destinationUnreachable:
      printf(" <--- #%u UNREACH", id);
      if (progressDetailed) {
	printf(": ");
	archtesterd_reportBriefConclusion();
      }
      printf("\n");
      break;
    case archtesterd_responseType_timeExceeded:
      printf(" <--- #%u TTL EXPIRED", id);
      if (progressDetailed) {
	printf(": ");
	archtesterd_reportBriefConclusion();
      }
      printf("\n");
      break;
    default:
      fatalf("invalid response type");
    }
  }
  
}

//
// Reporting progress: received other
//

static void
archtesterd_reportprogress_received_other() {
  
  if (progress) {
    printf(" <--- OTHER\n");
  }
  
}

//
// Search for what probes in a given TTL range
// have not been sent yet
//

static unsigned int
archtesterd_probesnotyetsentinrange(unsigned char minTtlValue,
				    unsigned char maxTtlValue) {

  int ttlsUsed[256];
  unsigned int count = 0;
  unsigned int id;
  unsigned int ttl;
  
  memset(ttlsUsed,0,sizeof(ttlsUsed));
  
  for (id = 0; id < ARCHTESTERD_MAX_PROBES; id++) {
    struct archtesterd_probe* probe = &probes[id];
    if (probe->used) {
      ttlsUsed[probe->hops] = 1;
    }
  }
  
  for (ttl = minTtlValue; ttl <= maxTtlValue; ttl++) {
    if (!ttlsUsed[ttl]) count++;
  }
  
  return(count);
}

//
// Can and should we continue the probing?
//

static int
archtesterd_shouldcontinue() {
  if (probesSent >= maxProbes) return(0);
  if (hopsMin == hopsMax) return(0);
  if (!archtesterd_probesnotyetsentinrange(hopsMin+1,hopsMax)) return(0);
  return(1);
}

//
// Send as many probes as we are allowed to send
//

static void
archtesterd_sendprobes(int sd,
		       struct sockaddr_in* destinationAddress,
		       struct sockaddr_in* sourceAddress) {

  unsigned int sent = 0;
  
  while (bucket > 0 && archtesterd_shouldcontinue()) {
    
    struct archtesterd_probe* probe;
    unsigned int packetLength;
    unsigned int expectedLen;
    archtesterd_idtype id;
    char* packet;
    
    //
    // Depending on algorithm, adjust behaviour
    //
    
    switch (algorithm) {
    case archtesterd_algorithms_random:
      debugf("before random selection, min = %u and max = %u", hopsMin, hopsMax);
      currentTtl = hopsMin + 1 + (rand() % (hopsMax - hopsMin));
      debugf("selected a random ttl %u in range %u..%u", currentTtl, hopsMin+1, hopsMax);
      break;
    case archtesterd_algorithms_sequential:
      if (probesSent > 0) currentTtl++;
      debugf("selected one larger ttl %u", currentTtl);
      break;
    case archtesterd_algorithms_reversesequential:
      if (probesSent > 0) currentTtl--;
      debugf("selected one smaller ttl %u", currentTtl);
      break;
    case archtesterd_algorithms_binarysearch:
      fatalf("binary search not implemented yet");
    default:
      fatalf("invalid internal algorithm identifier");
    }
    
    //
    // Create a packet
    //
    
    id = archtesterd_getnewid(currentTtl);
    expectedLen = ARCHTESTERD_IP4_HDRLEN + ARCHTESTERD_ICMP4_HDRLEN + icmpDataLength;
    probe = archtesterd_newprobe(id,currentTtl,expectedLen);
    if (probe == 0) {
      fatalf("cannot allocate a new probe entry");
    }
    archtesterd_constructicmp4packet(sourceAddress,
				     destinationAddress,
				     id,
				     currentTtl,
				     icmpDataLength,
				     &packet,
				     &packetLength);
    if (expectedLen != packetLength) {
      fatalf("expected and resulting packet lengths do not agree (%u vs. %u)",
	     expectedLen, packetLength);
    }
    
    //
    // Send the packet
    //
    
    archtesterd_sendpacket(sd,
			   packet,
			   packetLength, (struct sockaddr *)destinationAddress,
			   sizeof (struct sockaddr));
    if (sent > 0) archtesterd_reportprogress_sendingmore();
    archtesterd_reportprogress_sent(id,currentTtl);
    sent++;
    
  }
  
}

//
// The test main loop
//

static void
archtesterd_probingprocess(int sd,
			   int rd,
			   struct sockaddr_in* destinationAddress,
			   struct sockaddr_in* sourceAddress,
			   unsigned int startTtl) {
  
  enum archtesterd_responseType responseType;
  struct archtesterd_probe* responseToProbe;
  archtesterd_idtype responseId;
  int receivedPacketLength;
  char* receivedPacket;

  //
  // Initialize task counters
  //

  bucket = parallel;
  
  //
  // Adjust TTL if needed
  //
  
  debugf("startTtl %u, maxTtl %u", startTtl, maxTtl);
  if (startTtl > maxTtl) {
    startTtl = maxTtl;
    debugf("reset startTtl to %u", startTtl);
  }
  if (algorithm == archtesterd_algorithms_reversesequential && startTtl < maxTtl) {
    startTtl = maxTtl;
    debugf("reset startTtl to %u", startTtl);
  }
  currentTtl = startTtl;
  
  //
  // Loop
  //

  while (archtesterd_shouldcontinue()) {

    struct ip responseToIpHdr;
    struct icmp responseToIcmpHdr;

    //
    // Send as many probes as we can
    //

    archtesterd_sendprobes(sd,destinationAddress,sourceAddress);
    
    //
    // Wait for responses
    //
    
    if ((receivedPacketLength = archtesterd_receivepacket(rd, &receivedPacket)) > 0) {
      
      debugf("received a packet of %u bytes", receivedPacketLength);
      
      //
      // Verify response packet (that it is for us, long enough, etc.)
      //
      
      if (!archtesterd_validatepacket(receivedPacket,
				      receivedPacketLength,
				      &responseType,
				      &responseId,
				      &responseToIpHdr,
				      &responseToIcmpHdr)) {
      
	debugf("invalid packet, ignoring");
	archtesterd_reportprogress_received_other();
	
      } else if (!archtesterd_packetisforus(receivedPacket,
					    receivedPacketLength,
					    responseType,
					    sourceAddress,
					    destinationAddress,
					    &responseToIpHdr,
					    &responseToIcmpHdr)) {
	
	debugf("packet not for us, ignoring");
	archtesterd_reportprogress_received_other();
	
      } else {
	
	debugf("packet was for us, taking into account");
	
	//
	// Register the response into our own database
	//
	
	archtesterd_registerResponse(responseType, responseId, receivedPacketLength, &responseToProbe);
	archtesterd_reportprogress_received(responseType,
					    responseId,
					    responseToProbe != 0 ? 0 : responseToProbe->hops);
	
      }

    } else {

      //
      // We did not get a packet, but got a timeout instead
      //
      
    }
    
  }
  
}

//
// The main program for starting a test
//

static void
archtesterd_runtest(unsigned int startTtl,
		    const char* interface,
		    const char* destination) {

  struct sockaddr_in sourceAddress;
  struct sockaddr_in bindAddress;
  struct ifreq ifr;
  int hdrison = 1;
  int ifindex;
  int sd;
  int rd;
  
  //
  // Find out ifindex, own address, destination address
  //
  
  archtesterd_getifindex(interface,&ifindex,&ifr,&sourceAddress);
  archtesterd_getdestinationaddress(destination,&testDestinationAddress);
  
  //
  // Debugs
  //
  
  debugf("ifindex = %d", ifindex);
  debugf("source = %s", archtesterd_iptostring(&sourceAddress));
  debugf("destination = %s", archtesterd_iptostring(&testDestinationAddress));
  
  //
  // Get an output raw socket
  //
  
  if ((sd = socket (AF_INET, SOCK_RAW, IPPROTO_RAW)) < 0) {
    fatalp("socket() failed to get socket descriptor for using ioctl()");
  }
  
  if (setsockopt (sd, IPPROTO_IP, IP_HDRINCL, &hdrison, sizeof (hdrison)) < 0) {
    fatalp("setsockopt() failed to set IP_HDRINCL");
  }
  
  if (setsockopt (sd, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof (ifr)) < 0) {
    fatalp("setsockopt() failed to bind to interface");
  }

  //
  // Get an input raw socket
  //
  
  if ((rd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP)) == -1) {
    fatalp("cannot create input raw socket");
  }
  
  bindAddress.sin_family = AF_INET;
  bindAddress.sin_port = 0;
  bindAddress.sin_addr.s_addr = sourceAddress.sin_addr.s_addr;
  
  if (bind(rd, (struct sockaddr*) &bindAddress, sizeof(bindAddress)) == -1) {
    fatalp("cannot bind input raw socket");
  }

  //
  // Start the main loop
  //
  
  archtesterd_probingprocess(sd,rd,&testDestinationAddress,&sourceAddress,startTtl);
  
  //
  // Done. Return.
  //
  
}

//
// See if there were any unreachable errors
//

static unsigned int
archtesterd_replyresponses() {

  unsigned int count = 0;
  unsigned int id;
  
  for (id = 0; id < ARCHTESTERD_MAX_PROBES; id++) {
    struct archtesterd_probe* probe = &probes[id];
    if (probe->used &&
	probe->responded &&
	probe->responseType == archtesterd_responseType_echoResponse) {
      count++;
    }
  }
  
  return(count);
}

//
// See if there were any time exceeded errors
//

static unsigned int
archtesterd_timeexceededresponses() {

  unsigned int count = 0;
  unsigned int id;
  
  for (id = 0; id < ARCHTESTERD_MAX_PROBES; id++) {
    struct archtesterd_probe* probe = &probes[id];
    if (probe->used &&
	probe->responded &&
	probe->responseType == archtesterd_responseType_timeExceeded) {
      count++;
    }
  }
  
  return(count);
}

//
// See if there were any unreachable errors
//

static unsigned int
archtesterd_unreachableresponses() {

  unsigned int count = 0;
  unsigned int id;
  
  for (id = 0; id < ARCHTESTERD_MAX_PROBES; id++) {
    struct archtesterd_probe* probe = &probes[id];
    if (probe->used &&
	probe->responded &&
	probe->responseType == archtesterd_responseType_destinationUnreachable) {
      count++;
    }
  }
  
  return(count);
}

//
// Output the current state of (brief) conclusion (as much as we know)
// from the probing process
//

static void
archtesterd_reportBriefConclusion() {
  if (hopsMin == hopsMax) {
    printf("%u hops away\n", hopsMin);
  } else if (hopsMin == -1 && hopsMax >= maxTtl) {
    printf("unknown hops away\n");
  } else {
    printf("%u.. %u hops away",
	   hopsMin == -1 ? 0 : hopsMin,
	   hopsMax);
  }
}

//
// Output a conclusion (as much as we know) from the
// probing process
//

static void
archtesterd_reportConclusion() {
  
  unsigned int repl = archtesterd_replyresponses();
  unsigned int exc = archtesterd_timeexceededresponses();
  unsigned int unreach = archtesterd_unreachableresponses();
  const char* testDestinationAddressString = archtesterd_addrtostring(&testDestinationAddress.sin_addr);
  
  printf("%s (%s) is ", testDestination, testDestinationAddressString);
  if (hopsMin == hopsMax) {
    printf("%u hops away", hopsMin);
  } else if (hopsMin == -1 && hopsMax >= maxTtl) {
    printf("unknown hops away");
  } else {
    printf("between %u and %u hops away",
	   hopsMin == -1 ? 0 : hopsMin,
	   hopsMax);
  }
  
  if (repl == 0 && unreach > 0) {
    printf(", but may not be reachable\n");
  } else if (repl > 0 && unreach > 0) {
    printf(" and reachable, but also gives reachability errors\n");
  } else if (repl > 0 && unreach == 0) {
    printf(" and reachable\n");
  } else if (exc > 0) {
    printf(", not sure if it is reachable\n");
  } else {
    printf(", not sure if it is reachable as we got no ICMPs back at all\n");
  }
}

//
// Print out statistics related the process of probing
//

static void
archtesterd_reportStats() {
  
  unsigned int nProbes = 0;
  unsigned int nResponses = 0;
  unsigned int nEchoReplies = 0;
  unsigned int nDestinationUnreachables = 0;
  unsigned int nTimeExceededs = 0;
  unsigned int probeBytes = 0;
  unsigned int responseBytes = 0;
  unsigned int hopsused[256];
  unsigned int id;
  unsigned long shortestDelay = 0xffffffff;
  unsigned long longestDelay = 0;
  unsigned int ttl;
  int seenttl;
  
  memset(hopsused,0,sizeof(hopsused));
  for (id = 0; id < ARCHTESTERD_MAX_PROBES; id++) {
    struct archtesterd_probe* probe = &probes[id];
    if (probe->used) {
      nProbes++;
      hopsused[probe->hops]++;
      probeBytes += probe->probeLength;
      if (probe->responded) {
	nResponses++;
	responseBytes += probe->responseLength;
	if (probe->delayUSecs < shortestDelay) shortestDelay = probe->delayUSecs; 
	if (probe->delayUSecs > longestDelay) longestDelay = probe->delayUSecs; 
	switch (probe->responseType) {
	case archtesterd_responseType_echoResponse:
	  nEchoReplies++;
	  break;
	case archtesterd_responseType_destinationUnreachable:
	  nDestinationUnreachables++;
	  break;
	case archtesterd_responseType_timeExceeded:
	  nTimeExceededs++;
	  break;
	default:
	  fatalf("invalid response type");
	}
      }
    }
  }
  
  printf("\n");
  printf("Statistics:\n");
  printf("\n");
  printf("  %8u    probes sent out\n", nProbes);
  if (nProbes > 0) {
    printf("              on TTLs: ");
    seenttl = 0;
    for (ttl = 0; ttl < 256; ttl++) {
      if (hopsused[ttl]) {
	if (seenttl) printf(", ");
	printf("%u", ttl);
	if (hopsused[ttl] > 1) {
	  printf(" (%u times)", hopsused[ttl]);
	}
	seenttl = 1;
      }
    }
    printf("\n");
  }
  printf("%10u    bytes used in the probes\n", probeBytes);
  printf("  %8u    responses received\n", nResponses);
  printf("%10u    bytes used in the responses\n", responseBytes);
  printf("  %8u    echo replies received\n", nEchoReplies);
  printf("  %8u    destination unreachable errors received\n", nDestinationUnreachables);
  printf("  %8u    time exceeded errors received\n", nTimeExceededs);
  if (nResponses > 0) {
    printf("%10.4f    shortest response delay (ms)\n", ((float)shortestDelay / 1000.0));
    printf("%10.4f    longest response delay (ms)\n", ((float)longestDelay / 1000.0));
  }
}

int
main(int argc,
     char** argv) {

  //
  // Initialize
  //
  
  srand(time(0));
  
  //
  // Process arguments
  //
  
  argc--; argv++;
  while (argc > 0) {
    if (strcmp(argv[0],"-v") == 0) {
      printf("version 0.2\n");
      exit(0);
    } else if (strcmp(argv[0],"-q") == 0) {
      progress = 0;
    } else if (strcmp(argv[0],"-d") == 0) {
      debug = 1;
    } else if (strcmp(argv[0],"-np") == 0) {
      progress = 0;
      progressDetailed = 0;
    } else if (strcmp(argv[0],"-p") == 0) {
      progress = 1;
      progressDetailed = 1;
    } else if (strcmp(argv[0],"-y") == 0) {
      statistics = 1;
    } else if (strcmp(argv[0],"-s") == 0 && argc > 1 && isdigit(argv[1][0])) {
      icmpDataLength = atoi(argv[1]);
      argc--; argv++;
    } else if (strcmp(argv[0],"-maxttl") == 0 && argc > 1 && isdigit(argv[1][0])) {
      maxTtl = atoi(argv[1]);
      debugf("maxTtl set to %u", maxTtl);
      argc--; argv++;
    } else if (strcmp(argv[0],"-maxprobes") == 0 && argc > 1 && isdigit(argv[1][0])) {
      maxProbes = atoi(argv[1]);
      debugf("maxProbes set to %u", maxProbes);
      argc--; argv++;
    } else if (strcmp(argv[0],"-parallel") == 0 && argc > 1 && isdigit(argv[1][0])) {
      parallel = atoi(argv[1]);
      if (parallel < 1 || parallel >= 100) {
	fatalf("invalid number of parallel probes");
      }
      debugf("parallel set to %u", parallel);
      argc--; argv++;
    } else if (strcmp(argv[0],"-algorithm") == 0 && argc > 1) {
      if (strcmp(argv[1],"random") == 0) {
	algorithm = archtesterd_algorithms_random;
      } else if (strcmp(argv[1],"sequential") == 0) {
	algorithm = archtesterd_algorithms_sequential;
      } else if (strcmp(argv[1],"reversesequential") == 0) {
	algorithm = archtesterd_algorithms_reversesequential;
      } else if (strcmp(argv[1],"binarysearch") == 0) {
	algorithm = archtesterd_algorithms_binarysearch;
      } else {
	fatalf("invalid algorithm value %s (expecting %s)",
	       argv[1], archtesterd_algorithms_string);
      }
      argc--; argv++;
    } else if (strcmp(argv[0],"-i") == 0 && argc > 1) {
      interface = argv[1];
      argc--; argv++;
    } else if (strcmp(argv[0],"-startttl") == 0 && argc > 1 && isdigit(argv[1][0])) {
      startTtl = atoi(argv[1]);
      if (startTtl > 255) {
	fatalf("invalid TTL value");
      }
      argc--; argv++;
    } else if (argv[0][0] == '-') {
      fatalf("unrecognised option %s", argv[0]);
    } else if (argc > 1) {
      fatalf("too many arguments");
    } else {
      testDestination = argv[0];
    }
    argc--; argv++;
  }

  archtesterd_runtest(startTtl,
		      interface,
		      testDestination);

  if (conclusion) {
    archtesterd_reportConclusion();
  }
  
  if (statistics) {
    archtesterd_reportStats();
  }
  
  exit(0);
}
