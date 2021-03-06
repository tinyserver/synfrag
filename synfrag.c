/*
 * Copyright (c) 2012, Yahoo! Inc All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 *     Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.  Redistributions
 *     in binary form must reproduce the above copyright notice, this list
 *     of conditions and the following disclaimer in the documentation and/or
 *     other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,
 * OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Author: John Eaglesham
 */

#define __USE_BSD

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <err.h>
#include <string.h>
#include <netdb.h>

#ifdef __FreeBSD__
#include <netinet/in_systm.h>
#endif

#ifdef __linux
#define ETHERTYPE_IPV6 ETH_P_IPV6
#define __FAVOR_BSD
#endif

#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/ip_icmp.h>
#include <netinet/icmp6.h>
#include <netinet/tcp.h>
#include <netinet/if_ether.h>
#include <net/if.h>
#include <pcap.h>
#include <signal.h>
#include <sys/wait.h>
#include <getopt.h>
#include "checksums.h"
#include "flag_names.h"

#define DEFAULT_TIMEOUT_SECONDS 10
#define IP_FLAGS_OFFSET 13
#define SOURCE_PORT 44128
#define BIG_PACKET_SIZE 1500
#define PCAP_CAPTURE_LEN BIG_PACKET_SIZE
#define TCP_WINDOW 65535
/*
 * If this is ever big enough to exceed BIG_PACKET_SIZE when added with the
 * sizes of the other (IPv4/IPv6+destination options+fragmentation+padding)
 * headers, a buffer will overflow. So don't do that. 
 */
#define FRAGMENT_OFFSET_TO_BYTES 8
#define MINIMUM_FRAGMENT_SIZE FRAGMENT_OFFSET_TO_BYTES
#define MINIMUM_PACKET_SIZE 68

/* Save time typing/screen real estate. */
#define SIZEOF_ICMP6 sizeof( struct icmp6_hdr )
#define SIZEOF_TCP sizeof( struct tcphdr )
#define SIZEOF_IPV4 sizeof( struct ip )
#define SIZEOF_IPV6 sizeof( struct ip6_hdr )
#define SIZEOF_ETHER sizeof( struct ether_header )
/* This size is fixed but extends past the standard basic icmp header. */
#define SIZEOF_PING 8

/*
 * XXX We don't close the pcap device on failure anywhere. The OS will do it
 * for us, but it's impolite.
 */

/*
 * This might turn out to be stupid, but lets try having TCP tests be odd
 * numbered and ICMP even numbered. IPv4 tests will be <= 10 and IPv6 > 10.
 */
#define IS_TEST_TCP(x) ( x % 2 == 1 )
#define IS_TEST_ICMP(x) ( x % 2 == 0 )
#define IS_TEST_IPV4(x) ( x <= 10 )
#define IS_TEST_IPV6(x) ( x > 10 )
enum TEST_TYPE {
    TEST_IPV4_TCP = 1,
    TEST_FRAG_IPV4_TCP = 3,
    TEST_FRAG_OPTIONED_IPV4_TCP = 5,

    TEST_FRAG_IPV4_ICMP = 2,
    TEST_FRAG_OPTIONED_IPV4_ICMP = 4,

    TEST_IPV6_TCP = 11,
    TEST_FRAG_IPV6_TCP = 13,
    TEST_FRAG_OPTIONED_IPV6_TCP = 15,

    TEST_FRAG_IPV6_ICMP6 = 12,
    TEST_FRAG_OPTIONED_IPV6_ICMP6 = 14,

    TEST_INVALID = 0
};

/*
 * Items and their order in test_indexes needs to match test_names (but not
 * enum TEST_TYPE). 
 */
enum TEST_TYPE test_indexes[] = {
    TEST_IPV4_TCP,
    TEST_FRAG_IPV4_TCP,
    TEST_FRAG_IPV4_ICMP,
    TEST_FRAG_OPTIONED_IPV4_TCP,
    TEST_FRAG_OPTIONED_IPV4_ICMP,

    TEST_IPV6_TCP,
    TEST_FRAG_IPV6_TCP,
    TEST_FRAG_IPV6_ICMP6,
    TEST_FRAG_OPTIONED_IPV6_TCP,
    TEST_FRAG_OPTIONED_IPV6_ICMP6,

    0
};

char *test_names[] = {
    "v4-tcp",
    "v4-frag-tcp",
    "v4-frag-icmp",
    "v4-frag-optioned-tcp",
    "v4-frag-optioned-icmp",

    "v6-tcp",
    "v6-frag-tcp",
    "v6-frag-icmp6",
    "v6-frag-optioned-tcp",
    "v6-frag-optioned-icmp6",

    NULL
};

pcap_t *pcap;
pid_t listener_pid;
int pfd[2];

#ifdef SIOCGIFHWADDR
void fill_interface_mac( char *dest, char *interface )
{
    int fd;
    struct ifreq ifr;

    fd = socket( AF_INET, SOCK_DGRAM, 0 );
    if ( fd == -1 )
        err( 1, "socket failed" );

    ifr.ifr_addr.sa_family = AF_INET;
    strncpy( ifr.ifr_name, interface, IFNAMSIZ - 1 );

    if ( ioctl( fd, SIOCGIFHWADDR, &ifr ) == -1 ) {
        close( fd );
        err( 1, "ioctl failed" );
    }
    close( fd );

    memcpy( dest, ifr.ifr_hwaddr.sa_data, ETHER_ADDR_LEN );
}
#elif __FreeBSD__
#include <ifaddrs.h>
#include <net/if_dl.h>
void fill_interface_mac( char *dest, char *interface )
{
    struct ifaddrs *ifap;

    if ( getifaddrs( &ifap ) == 0 ) {
        struct ifaddrs *p;
        for ( p = ifap; p; p = p->ifa_next ) {
            if ( ( p->ifa_addr->sa_family == AF_LINK ) && ( strcmp( p->ifa_name, interface ) == 0 ) ) {
                struct sockaddr_dl* sdp = (struct sockaddr_dl*) p->ifa_addr;
                memcpy( dest, sdp->sdl_data + sdp->sdl_nlen, 6 );
                freeifaddrs( ifap );
                return;
            }
        }
        freeifaddrs(ifap);
    }
    errx( 1, "Failed to get MAC for interface %s", interface );
}
#else
#error Do not know how to get MAC address on this platform.
#endif

void *malloc_check( int size )
{
    void *r = malloc( size );
    if ( r == NULL ) err( 1, "malloc" );
    return r;
}

unsigned short fix_up_destination_options_length( unsigned short optlen )
{
    /*
     * Per RFC 2460, a destination options header must have a payload size
     * that is: ( multiple of 8 ) - 2. We fix that up here by only ever
     * increasing the size (though the algorithm would be simpler if we were
     * willing to decrease it. There's no good reason not to decrease it, I
     * just made a decision.
     */
    if ( optlen % 8 != 6 ) {
        int x = 6 - ( optlen % 8 );
        if ( x < 0 ) x += 8;
        optlen += x;
    }
    return optlen;
}

void print_ethh( struct ether_header *ethh )
{
    printf( "Ethernet Frame, ethertype 0x%04X (%s)\n",
        ntohs( ethh->ether_type ),
        ether_protocol_to_name( ntohs( ethh->ether_type ) )
    );

    printf( " Src MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
        ethh->ether_shost[0],
        ethh->ether_shost[1],
        ethh->ether_shost[2],
        ethh->ether_shost[3],
        ethh->ether_shost[4],
        ethh->ether_shost[5] );

    printf( " Dest MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
        ethh->ether_dhost[0],
        ethh->ether_dhost[1],
        ethh->ether_dhost[2],
        ethh->ether_dhost[3],
        ethh->ether_dhost[4],
        ethh->ether_dhost[5] );

    printf( "\n" );
}

void print_iph( struct ip *iph )
{
    char srcbuf[INET_ADDRSTRLEN];
    char dstbuf[INET_ADDRSTRLEN];
    char *flag_names = ip_flags_to_names( ntohs( iph->ip_off ) >> IP_FLAGS_OFFSET );

    if ( inet_ntop( AF_INET, &iph->ip_src, (char *) &srcbuf, INET_ADDRSTRLEN ) == NULL ) err( 1, "inet_ntop failed" );
    if ( inet_ntop( AF_INET, &iph->ip_dst, (char *) &dstbuf, INET_ADDRSTRLEN ) == NULL ) err( 1, "inet_ntop failed" );

    printf( "IPv4 Packet:\n\
 Src IP: %s\n\
 Dst IP: %s\n\
 Protocol: %i (%s)\n\
 Frag Offset: %i (%i bytes)\n\
 Flags: %i (%s)\n\
 Iphl: %i (%i bytes)\n\
\n",
        (char *) &srcbuf,
        (char *) &dstbuf,
        iph->ip_p,
        ip_protocol_to_name( iph->ip_p ),
        ntohs( iph->ip_off ) & 0x1FFF,
        ( ntohs( iph->ip_off ) & 0x1FFF ) * FRAGMENT_OFFSET_TO_BYTES,
        ntohs( iph->ip_off ) >> IP_FLAGS_OFFSET,
        flag_names,
        iph->ip_hl,
        iph->ip_hl * 4
    );
    free( flag_names );
}

void print_ip6h( struct ip6_hdr *ip6h )
{
    char srcbuf[INET6_ADDRSTRLEN];
    char dstbuf[INET6_ADDRSTRLEN];

    if ( inet_ntop( AF_INET6, &ip6h->ip6_src, (char *) &srcbuf, INET6_ADDRSTRLEN ) == NULL ) err( 1, "inet_ntop failed" );
    if ( inet_ntop( AF_INET6, &ip6h->ip6_dst, (char *) &dstbuf, INET6_ADDRSTRLEN ) == NULL ) err( 1, "inet_ntop failed" );

    printf( "IPv6 Packet:\n\
 Src IP: %s\n\
 Dst IP: %s\n\
 Protocol: %i (%s)\n\
 Payload Len: %i\n\
\n",
        (char *) &srcbuf,
        (char *) &dstbuf,
        ip6h->ip6_nxt,
        ip_protocol_to_name( ip6h->ip6_nxt ),
        ntohs( ip6h->ip6_plen )
    );
}

void print_icmph( struct icmp *icmph )
{
    printf( "ICMP Packet:\n\
 Type: %i (%s)\n\
 Code: %i (%s)\n",
        icmph->icmp_type,
        icmp_type_to_name( icmph->icmp_type ),
        icmph->icmp_code,
        icmp_code_to_name( icmph->icmp_type, icmph->icmp_code )
    );
    if ( ( icmph->icmp_type == ICMP_ECHO || icmph->icmp_type == ICMP_ECHOREPLY ) && ( icmph->icmp_code == 0 ) ) {
        printf( " Echo id: %i\n", htons( icmph->icmp_id ) );
    }
    printf( "\n" );
}

void print_icmp6h( struct icmp6_hdr *icmp6h )
{
    printf( "ICMPv6 Packet:\n\
 Type: %i (%s)\n\
 Code: %i (%s)\n",
        icmp6h->icmp6_type,
        icmp6_type_to_name( icmp6h->icmp6_type ),
        icmp6h->icmp6_code,
        icmp6_code_to_name( icmp6h->icmp6_type, icmp6h->icmp6_code )
    );
    if ( ( icmp6h->icmp6_type == ICMP6_ECHO_REQUEST || icmp6h->icmp6_type == ICMP6_ECHO_REPLY ) && ( icmp6h->icmp6_code == 0 ) ) {
        printf( " Echo id: %i\n", htons( icmp6h->icmp6_id ) );
    }
    printf( "\n" );
}

void print_tcph( struct tcphdr *tcph )
{
    char *tcp_flags = tcp_flags_to_names( tcph->th_flags );
    printf( "TCP Packet:\n\
 Src Port: %u\n\
 Dst Port: %u\n\
 Seq Num: %u\n\
 Ack Num: %u\n\
 Flags: %i (%s)\n\
\n",
        ntohs( tcph->th_sport ),
        ntohs( tcph->th_dport ),
        ntohl( tcph->th_seq ),
        ntohl( tcph->th_ack ),
        tcph->th_flags,
        tcp_flags
    );
    free( tcp_flags );
}

void build_ethernet( struct ether_header *ethh, char *interface, char *remote_mac, short int ethertype )
{
    fill_interface_mac( (char *) &ethh->ether_shost, interface );

    if ( sscanf( remote_mac, "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx",
            &ethh->ether_dhost[0],
            &ethh->ether_dhost[1],
            &ethh->ether_dhost[2],
            &ethh->ether_dhost[3],
            &ethh->ether_dhost[4],
            &ethh->ether_dhost[5] ) != 6 ) {
        errx( 1, "Unable to parse remote MAC address" );
    }

    ethh->ether_type = htons( ethertype );
    print_ethh( ethh );
}

void build_tcp_syn( void *iph, struct tcphdr *tcph, unsigned short srcport, unsigned short dstport )
{
    tcph->th_sport = htons( srcport );
    tcph->th_dport = htons( dstport );
    tcph->th_seq = htonl( rand() );
    tcph->th_ack = 0;
    tcph->th_x2 = 0;
    tcph->th_off = SIZEOF_TCP / 4;
    tcph->th_flags = TH_SYN;
    tcph->th_win = TCP_WINDOW;
    tcph->th_sum = 0;
    tcph->th_urp = 0;
    if ( do_checksum( iph, IPPROTO_TCP, SIZEOF_TCP ) != 1 )
        errx( 1, "Unable to compute checksum (build_tcp_syn)." );

    print_tcph( tcph );
}

void build_icmp_ping( void *iph, struct icmp *icmph, unsigned short payload_length )
{
    icmph->icmp_type = ICMP_ECHO;
    icmph->icmp_code = 0;
    icmph->icmp_cksum = 0;
    icmph->icmp_id = htons( SOURCE_PORT );
    icmph->icmp_seq = htons( 1 );
    memset( (char *) icmph + SIZEOF_PING, 0x01, payload_length );
    if ( do_checksum( iph, IPPROTO_ICMP, SIZEOF_PING + payload_length ) != 1 )
        errx( 1, "Unable to compute checksum (build_icmp_ping)." );

    print_icmph( icmph );
}

void build_icmp6_ping( void *iph, struct icmp6_hdr *icmp6h, unsigned short payload_length )
{
    icmp6h->icmp6_type = ICMP6_ECHO_REQUEST;
    icmp6h->icmp6_code = 0;
    icmp6h->icmp6_cksum = 0;
    icmp6h->icmp6_id = htons( SOURCE_PORT );
    icmp6h->icmp6_seq = htons( 1 );
    memset( (char *) icmp6h + SIZEOF_ICMP6, 0x01, payload_length );
    if ( do_checksum( iph, IPPROTO_ICMPV6, SIZEOF_ICMP6 + payload_length ) != 1 )
        errx( 1, "Unable to compute checksum (build_icmp6_ping)." );

    print_icmp6h( icmp6h );
}

void build_bare_ipv4( struct ip *iph, char *srcip, char *dstip, unsigned char protocol )
{
    iph->ip_v = 4;
    iph->ip_hl = 5;
    iph->ip_tos = 0;
    iph->ip_len = htons( SIZEOF_IPV4 + SIZEOF_TCP );
    iph->ip_id = 0;
    iph->ip_off = 0;
    iph->ip_ttl = IPDEFTTL;
    iph->ip_p = protocol;
    iph->ip_sum = 0;
    iph->ip_src.s_addr = inet_addr(srcip);
    iph->ip_dst.s_addr = inet_addr(dstip);
}

void build_ipv4( struct ip *iph, char *srcip, char *dstip, unsigned char protocol )
{
    build_bare_ipv4( iph, srcip, dstip, protocol );
    if ( do_checksum( (char *) iph, IPPROTO_IP, iph->ip_hl * 4 ) != 1 )
        errx( 1, "Unable to compute checksum (build_ipv4)." );

    print_iph( iph );
}

void build_ipv4_short_frag1( struct ip *iph, char *srcip, char *dstip, unsigned char protocol, unsigned short fragid )
{
    build_bare_ipv4( iph, srcip, dstip, protocol );
    iph->ip_off = htons( 1 << IP_FLAGS_OFFSET ); /* Set More Fragments (MF) bit */
    iph->ip_id = htons( fragid );
    iph->ip_len = htons( SIZEOF_IPV4 + MINIMUM_FRAGMENT_SIZE );
    if ( do_checksum( (char *) iph, IPPROTO_IP, iph->ip_hl * 4 ) != 1 )
        errx( 1, "Unable to compute checksum (build_ipv4_short_frag1)." );

    print_iph( iph );
}

void build_ipv4_frag2( struct ip *iph, char *srcip, char *dstip, unsigned char protocol, unsigned short fragid, unsigned short payload_length )
{
    build_bare_ipv4( iph, srcip, dstip, protocol );
    iph->ip_off = htons( 1 );
    iph->ip_id = htons( fragid );
    iph->ip_len = htons( SIZEOF_IPV4 + payload_length );
    if ( do_checksum( (char *) iph, IPPROTO_IP, iph->ip_hl * 4 ) != 1 )
        errx( 1, "Unable to compute checksum (build_ipv4_frag2)." );

    print_iph( iph );
}

void build_ipv4_optioned_frag1( struct ip *iph, char *srcip, char *dstip, unsigned char protocol, unsigned short fragid, unsigned short optlen )
{
    build_bare_ipv4( iph, srcip, dstip, protocol );
    iph->ip_off = htons( 1 << IP_FLAGS_OFFSET ); /* Set More Fragments (MF) bit */
    iph->ip_id = htons( fragid );
    iph->ip_len = htons( SIZEOF_IPV4 + optlen + MINIMUM_FRAGMENT_SIZE );

    if ( optlen % 4 != 0 ) errx( 1, "optlen must be a multiple of 4" );
    iph->ip_hl = 5 + optlen;

    /* Pad with NOP's and then end-of-padding option. */
    memset( (char *) iph + SIZEOF_IPV4, 0x01, optlen );
    *( (char *) iph + SIZEOF_IPV4 + optlen ) = 0;
    if ( do_checksum( (char *) iph, IPPROTO_IP, iph->ip_hl * 4 ) != 1 )
        errx( 1, "Unable to compute checksum (build_ipv4_optioned_frag1)." );

    print_iph( iph );
}

void build_ipv6( struct ip6_hdr *ip6h, char *srcip, char *dstip, unsigned char protocol, unsigned short payload_length )
{
    /* 4 bits version, 8 bits TC, 20 bits flow-ID. We only set the version bits. */
    ip6h->ip6_flow = htonl( 0x06 << 28 );
    ip6h->ip6_plen = htons( payload_length );
    ip6h->ip6_hlim = 64;
    ip6h->ip6_nxt = protocol;
    if ( !inet_pton( AF_INET6, srcip, &ip6h->ip6_src ) ) errx( 1, "Invalid source address" );
    if ( !inet_pton( AF_INET6, dstip, &ip6h->ip6_dst ) ) errx( 1, "Invalid source address" );

    print_ip6h( ip6h );
}

void build_ipv6_short_frag1( struct ip6_hdr *ip6h, char *srcip, char *dstip, unsigned char protocol, unsigned short fragid )
{
    struct ip6_frag *fragh = (struct ip6_frag *) ( (char *)ip6h + SIZEOF_IPV6 );

    /* 4 bits version, 8 bits TC, 20 bits flow-ID. We only set the version bits. */
    ip6h->ip6_flow = htonl( 0x06 << 28 );
    ip6h->ip6_plen = htons( sizeof( struct ip6_frag ) + MINIMUM_FRAGMENT_SIZE );
    ip6h->ip6_hlim = 64;
    ip6h->ip6_nxt = IPPROTO_FRAGMENT;
    if ( !inet_pton( AF_INET6, srcip, &ip6h->ip6_src ) ) errx( 1, "Invalid source address" );
    if ( !inet_pton( AF_INET6, dstip, &ip6h->ip6_dst ) ) errx( 1, "Invalid source address" );

    fragh->ip6f_reserved = 0;
    fragh->ip6f_nxt = protocol;
    fragh->ip6f_ident = htons( fragid );
    fragh->ip6f_offlg = IP6F_MORE_FRAG;

    print_ip6h( ip6h );
}

void build_ipv6_optioned_frag1( struct ip6_hdr *ip6h, char *srcip, char *dstip, unsigned char protocol, unsigned short fragid, unsigned short optlen )
{
    struct ip6_dest *desth = (struct ip6_dest *) ( (char *)ip6h + SIZEOF_IPV6 );
    struct ip6_frag *fragh = (struct ip6_frag *) ( (char *)ip6h + SIZEOF_IPV6 + sizeof( struct ip6_dest ) + optlen );

    /* 4 bits version, 8 bits TC, 20 bits flow-ID. We only set the version bits. */
    ip6h->ip6_flow = htonl( 0x06 << 28 );
    ip6h->ip6_plen = htons( sizeof( struct ip6_dest ) + optlen + sizeof( struct ip6_frag ) + MINIMUM_FRAGMENT_SIZE );
    ip6h->ip6_hlim = 64;
    ip6h->ip6_nxt = IPPROTO_DSTOPTS;
    if ( !inet_pton( AF_INET6, srcip, &ip6h->ip6_src ) ) errx( 1, "Invalid source address" );
    if ( !inet_pton( AF_INET6, dstip, &ip6h->ip6_dst ) ) errx( 1, "Invalid source address" );

    if ( optlen == 0 || optlen % 8 != 6 ) errx( 1, "optlen value not supported" );
    desth->ip6d_nxt = IPPROTO_FRAGMENT;
    desth->ip6d_len = optlen / 8;

    *( (char *) desth + sizeof( struct ip6_dest ) ) = 1;
    *( (char *) desth + sizeof( struct ip6_dest ) + 1 ) = optlen - 2;
    memset( (char *) desth + sizeof( struct ip6_dest ) + 2, 0, optlen - 2 );

    fragh->ip6f_reserved = 0;
    fragh->ip6f_nxt = protocol;
    fragh->ip6f_ident = htons( fragid );
    fragh->ip6f_offlg = IP6F_MORE_FRAG;

    print_ip6h( ip6h );
}

void build_ipv6_frag2( struct ip6_hdr *ip6h, char *srcip, char *dstip, unsigned char protocol, unsigned short fragid, unsigned short payload_length )
{
    struct ip6_frag *fragh = (struct ip6_frag *) ( (char *)ip6h + SIZEOF_IPV6 );

    /* 4 bits version, 8 bits TC, 20 bits flow-ID. We only set the version bits. */
    ip6h->ip6_flow = htonl( 0x06 << 28 );
    ip6h->ip6_plen = htons( payload_length + sizeof( struct ip6_frag ) );
    ip6h->ip6_hlim = 64;
    ip6h->ip6_nxt = IPPROTO_FRAGMENT;
    if ( !inet_pton( AF_INET6, srcip, &ip6h->ip6_src ) ) errx( 1, "Invalid source address" );
    if ( !inet_pton( AF_INET6, dstip, &ip6h->ip6_dst ) ) errx( 1, "Invalid source address" );

    fragh->ip6f_reserved = 0;
    fragh->ip6f_nxt = protocol;
    fragh->ip6f_ident = htons( fragid );
    fragh->ip6f_offlg = htons( 1 << 3 );

    print_ip6h( ip6h );
}

/* Returns the layer 4 header if we found one. */
char *print_a_packet( int len, char *packet_data, unsigned short wanted_type )
{
    struct ip *iph;
    struct ip6_hdr *ip6h;
    size_t s;
    struct ether_header *ethh = (struct ether_header *) packet_data;
    unsigned short found_type;
    char *found_header;

    if ( ntohs( ethh->ether_type ) == ETHERTYPE_IP ) {
        iph = (struct ip *) ( packet_data + SIZEOF_ETHER );
        s = SIZEOF_ETHER + ( iph->ip_hl * 4 );
        if ( s > len ) errx( 1, "Reply too short (IPv4)" );
        print_iph( iph );
        if ( iph->ip_p == IPPROTO_TCP ) {
            if ( s + SIZEOF_TCP > len ) errx( 1, "Reply too short" );
            print_tcph( (struct tcphdr *) ( packet_data + s ) );
            found_type = IPPROTO_TCP;
            found_header = packet_data + s;
        } else if ( iph->ip_p == IPPROTO_ICMP ) {
            if ( s + SIZEOF_PING > len ) errx( 1, "Reply too short" );
            print_icmph( (struct icmp *) ( packet_data + s ) );
            found_type = IPPROTO_ICMP;
            found_header = packet_data + s;
        } else {
            errx( 1, "Unknown reply received (ip protocol %i)", iph->ip_p );
        }

    } else if ( ntohs( ethh->ether_type ) == ETHERTYPE_IPV6 ) {
        ip6h = (struct ip6_hdr *) ( packet_data + SIZEOF_ETHER );
        s = SIZEOF_ETHER + SIZEOF_IPV6;
        if ( s > len ) errx( 1, "Reply too short (IPv6)" );
        print_ip6h( ip6h );
        if ( ip6h->ip6_nxt == IPPROTO_TCP ) {
            if ( s + SIZEOF_TCP > len ) errx( 1, "Reply too short" );
            print_tcph( (struct tcphdr *) ( packet_data + s ) );
            found_type = IPPROTO_TCP;
            found_header = packet_data + s;
        } else if ( ip6h->ip6_nxt == IPPROTO_ICMPV6 ) {
            if ( s + SIZEOF_ICMP6 > len ) errx( 1, "Reply too short" );
            print_icmp6h( (struct icmp6_hdr *) ( packet_data + s ) );
            found_type = IPPROTO_ICMPV6;
            found_header = packet_data + s;
        } else {
            errx( 1, "Unknown reply received (ip6 next header %i)", ip6h->ip6_nxt );
        }

    } else {
        errx( 1, "Unknown reply received (ethertype %i)", ntohs( ethh->ether_type ) );
    }

    if ( wanted_type == found_type ) return found_header;
    return NULL;
}

int receive_a_packet( char *srcip, char *dstip, unsigned short srcport, unsigned short dstport, enum TEST_TYPE test_type, char **packet_buf, long receive_timeout )
{
    struct pcap_pkthdr *received_packet_pcap;
    struct bpf_program pcap_filter;
    char *received_packet_data;
/*
 * My back-of-the-napkin for the maximum length for the ipv6 filter string
 * below + 1 byte for the trailing NULL 
 */
#define FILTER_STR_LEN 203 
    char filter_str[FILTER_STR_LEN];
    int r, fd;
    fd_set select_me;
    struct timeval ts;

    /*
     * Something prior to now should have validated srcip and dstip are valid
     * IP addresses, we hope. Napkin math says we shouldn't even be close to
     * overflowing our buffer.
     */
    if ( IS_TEST_IPV4( test_type ) ) {
        r = snprintf(
            (char *) &filter_str,
            FILTER_STR_LEN,
            "src %s and dst %s and (icmp or (tcp and src port %i and dst port %i))",
            srcip,
            dstip,
            srcport,
            dstport
        );
    } else {
        r = snprintf(
            (char *) &filter_str,
            FILTER_STR_LEN,
            /* Attempt to ignore ICMP6 neighbor solicitation/advertisement */
            "src %s and dst %s and ((icmp6 and ip6[40] != 135 and ip6[40] != 136) or (tcp and src port %i and dst port %i))",
            srcip,
            dstip,
            srcport,
            dstport
        );
    }
    if ( r < 0 || r >= FILTER_STR_LEN ) errx( 1, "snprintf for pcap filter failed" );
    if ( pcap_compile( pcap, &pcap_filter, (char *) &filter_str, 1, 0 ) == -1 )
        errx( 1, "pcap_compile failed: %s", pcap_geterr( pcap ) );
    if ( pcap_setfilter( pcap, &pcap_filter ) == -1 )
        errx( 1, "pcap_setfilter failed: %s", pcap_geterr( pcap ) );
    pcap_freecode( &pcap_filter );

    if ( ( fd = pcap_fileno( pcap ) ) == -1 )
        errx( 1, "pcap_fileno failed" );

    FD_ZERO( &select_me );
    FD_SET( fd, &select_me );

    ts.tv_sec = receive_timeout;
    ts.tv_usec = 0;

    /*
     * Signal we're ready to go. Still a race condition. I don't see how to
     * work around this with pcap. 
     */
    write( pfd[1], ".", 1 );
    r = select( fd + 1, &select_me, NULL, NULL, &ts );
    /* Timed out */
    if ( r == 0 ) return 0;

    r = pcap_next_ex( pcap, &received_packet_pcap, (const unsigned char **) &received_packet_data );

    /* Error or pcap_next_ex timed out (should never happen) */
    if ( r < 1 ) return 0;
    if ( received_packet_pcap->len > received_packet_pcap->caplen ) errx( 1, "pcap didn't capture the whole packet." );

    *packet_buf = (char *) received_packet_data;
    return received_packet_pcap->len;
}

int check_received_packet( int buf_len, char *packet_buf, enum TEST_TYPE test_type ) {
    struct ether_header *received_packet_data = (struct ether_header *) packet_buf;
    struct tcphdr *tcph;
    struct icmp *icmph;
    struct icmp6_hdr *icmp6h;

    if ( IS_TEST_IPV4( test_type ) && IS_TEST_ICMP( test_type ) ) {
        icmph = (struct icmp *) print_a_packet( buf_len, (char *) received_packet_data, IPPROTO_ICMP );
        if ( !icmph ) return 0;
        if ( icmph->icmp_type == ICMP_ECHOREPLY && icmph->icmp_id == htons( SOURCE_PORT ) ) return 1;
    } else if ( IS_TEST_IPV6( test_type ) && IS_TEST_ICMP( test_type ) ) {
        icmp6h = (struct icmp6_hdr *) print_a_packet( buf_len, (char *) received_packet_data, IPPROTO_ICMPV6 );
        if ( !icmp6h ) return 0;
        if ( icmp6h->icmp6_type == ICMP6_ECHO_REPLY && icmp6h->icmp6_id == htons( SOURCE_PORT ) ) return 1;
    } else { /* Assume pcap picked the right address family for our packet. */
        tcph = (struct tcphdr *) print_a_packet( buf_len, (char *) received_packet_data, IPPROTO_TCP );
        if ( !tcph ) return 0;
        if ( ( tcph->th_flags & ( TH_SYN|TH_ACK ) ) && !( tcph->th_flags & TH_RST ) ) {
            return 1;
        }
    }
    printf( "Received reply but it wasn't what we were hoping for.\n" );
    return 0;
}

/* IPv4 tests. */
void do_ipv4_syn( char *interface, char *srcip, char *dstip, char *dstmac, unsigned short dstport )
{
    struct ip *iph;
    struct tcphdr *tcph;
    struct ether_header *ethh;
    int packet_size;

    packet_size = SIZEOF_ETHER + SIZEOF_TCP + SIZEOF_IPV4;

    ethh = (struct ether_header *) malloc_check( packet_size );
    iph = (struct ip *) ( (char *) ethh + SIZEOF_ETHER );
    tcph = (struct tcphdr *) ( (char *) iph + SIZEOF_IPV4 );

    build_ethernet( ethh, interface, dstmac, ETHERTYPE_IP );
    build_ipv4( iph, srcip, dstip, IPPROTO_TCP );
    build_tcp_syn( iph, tcph, SOURCE_PORT, dstport );

    if ( pcap_inject( pcap, ethh, packet_size ) != packet_size ) errx( 1, "pcap_inject" );
    free( ethh );
}

void do_ipv4_short_tcp_frag( char *interface, char *srcip, char *dstip, char *dstmac, unsigned short dstport )
{
    struct ip *iph;
    struct tcphdr *tcph;
    struct ether_header *ethh;
    int packet_size;
    unsigned short fragid = rand();

    packet_size = SIZEOF_ETHER + SIZEOF_IPV4 + MINIMUM_FRAGMENT_SIZE;

    ethh = (struct ether_header *) malloc_check( BIG_PACKET_SIZE );
    iph = (struct ip *) ( (char *) ethh + SIZEOF_ETHER );
    tcph = (struct tcphdr *) ( (char *) iph + SIZEOF_IPV4 );

    build_ethernet( ethh, interface, dstmac, ETHERTYPE_IP );
    build_ipv4_short_frag1( iph, srcip, dstip, IPPROTO_TCP, fragid );
    build_tcp_syn( iph, tcph, SOURCE_PORT, dstport );

    if ( pcap_inject( pcap, ethh, packet_size ) != packet_size ) errx( 1, "pcap_inject" );

    packet_size = SIZEOF_ETHER + SIZEOF_IPV4 + SIZEOF_TCP - MINIMUM_FRAGMENT_SIZE;

    build_ipv4_frag2( iph, srcip, dstip, IPPROTO_TCP, fragid, SIZEOF_TCP - MINIMUM_FRAGMENT_SIZE );
    memmove( tcph, (char *) tcph + MINIMUM_FRAGMENT_SIZE, SIZEOF_TCP - MINIMUM_FRAGMENT_SIZE );

    if ( pcap_inject( pcap, ethh, packet_size ) != packet_size ) errx( 1, "pcap_inject" );
    free( ethh );
}

void do_ipv4_short_icmp_frag( char *interface, char *srcip, char *dstip, char *dstmac )
{
    struct ip *iph;
    struct icmp *icmph;
    struct ether_header *ethh;
    int packet_size;
    unsigned short fragid = rand();
    unsigned short pinglen = 40;

    packet_size = SIZEOF_ETHER + SIZEOF_IPV4 + MINIMUM_FRAGMENT_SIZE;

    ethh = (struct ether_header *) malloc_check( BIG_PACKET_SIZE );
    iph = (struct ip *) ( (char *) ethh + SIZEOF_ETHER );
    icmph = (struct icmp *) ( (char *) iph + SIZEOF_IPV4 );

    build_ethernet( ethh, interface, dstmac, ETHERTYPE_IP );
    build_ipv4_short_frag1( iph, srcip, dstip, IPPROTO_ICMP, fragid );
    build_icmp_ping( iph, icmph, pinglen );

    if ( pcap_inject( pcap, ethh, packet_size ) != packet_size ) errx( 1, "pcap_inject" );

    packet_size = SIZEOF_ETHER + SIZEOF_IPV4 + SIZEOF_PING + pinglen - MINIMUM_FRAGMENT_SIZE;

    build_ipv4_frag2( iph, srcip, dstip, IPPROTO_ICMP, fragid, SIZEOF_PING + pinglen - MINIMUM_FRAGMENT_SIZE );
    memmove( icmph, (char *) icmph + MINIMUM_FRAGMENT_SIZE, SIZEOF_PING + pinglen - MINIMUM_FRAGMENT_SIZE );

    if ( pcap_inject( pcap, ethh, packet_size ) != packet_size ) errx( 1, "pcap_inject" );
    free( ethh );
}

void do_ipv4_optioned_tcp_frag( char *interface, char *srcip, char *dstip, char *dstmac, unsigned short dstport )
{
    struct ip *iph;
    struct tcphdr *tcph, *tcph_optioned;
    struct ether_header *ethh;
    int packet_size;
    unsigned short fragid = rand();
    unsigned short optlen = 40; /* Multiple of 4. */

    packet_size = SIZEOF_ETHER + SIZEOF_IPV4 + optlen + MINIMUM_FRAGMENT_SIZE;

    ethh = (struct ether_header *) malloc_check( BIG_PACKET_SIZE );
    iph = (struct ip *) ( (char *) ethh + SIZEOF_ETHER );
    tcph = (struct tcphdr *) ( (char *) iph + SIZEOF_IPV4 );
    tcph_optioned = (struct tcphdr *) ( (char *) tcph + optlen );

    build_ethernet( ethh, interface, dstmac, ETHERTYPE_IP );
    build_ipv4_optioned_frag1( iph, srcip, dstip, IPPROTO_TCP, fragid, optlen );
    build_tcp_syn( iph, tcph_optioned, SOURCE_PORT, dstport );

    if ( pcap_inject( pcap, ethh, packet_size ) != packet_size ) errx( 1, "pcap_inject" );

    packet_size = SIZEOF_ETHER + SIZEOF_IPV4 + SIZEOF_TCP - MINIMUM_FRAGMENT_SIZE;

    build_ipv4_frag2( iph, srcip, dstip, IPPROTO_TCP, fragid, SIZEOF_TCP - MINIMUM_FRAGMENT_SIZE );
    memmove( tcph, (char *) tcph_optioned + MINIMUM_FRAGMENT_SIZE, SIZEOF_TCP - MINIMUM_FRAGMENT_SIZE );

    if ( pcap_inject( pcap, ethh, packet_size ) != packet_size ) errx( 1, "pcap_inject" );
    free( ethh );
}

void do_ipv4_optioned_icmp_frag( char *interface, char *srcip, char *dstip, char *dstmac )
{
    struct ip *iph;
    struct icmp *icmph, *icmph_optioned;
    struct ether_header *ethh;
    int packet_size;
    unsigned short fragid = rand();
    unsigned short optlen = 40; /* Multiple of 4. */
    unsigned short pinglen = 40;

    packet_size = SIZEOF_ETHER + SIZEOF_IPV4 + optlen + MINIMUM_FRAGMENT_SIZE;

    ethh = (struct ether_header *) malloc_check( BIG_PACKET_SIZE );
    iph = (struct ip *) ( (char *) ethh + SIZEOF_ETHER );
    icmph = (struct icmp *) ( (char *) iph + SIZEOF_IPV4 );
    icmph_optioned = (struct icmp *) ( (char *) icmph + optlen );

    build_ethernet( ethh, interface, dstmac, ETHERTYPE_IP );
    build_ipv4_optioned_frag1( iph, srcip, dstip, IPPROTO_ICMP, fragid, optlen );
    build_icmp_ping( iph, icmph_optioned, pinglen );

    if ( pcap_inject( pcap, ethh, packet_size ) != packet_size ) errx( 1, "pcap_inject" );

    packet_size = SIZEOF_ETHER + SIZEOF_IPV4 + SIZEOF_PING + pinglen;

    build_ipv4_frag2( iph, srcip, dstip, IPPROTO_ICMP, fragid, SIZEOF_PING - MINIMUM_FRAGMENT_SIZE + pinglen );
    memmove( icmph, (char *) icmph_optioned + MINIMUM_FRAGMENT_SIZE, SIZEOF_PING + pinglen - MINIMUM_FRAGMENT_SIZE );

    if ( pcap_inject( pcap, ethh, packet_size ) != packet_size ) errx( 1, "pcap_inject" );
    free( ethh );
}

/* IPv6 tests. */
void do_ipv6_syn( char *interface, char *srcip, char *dstip, char *dstmac, unsigned short dstport )
{
    struct ip6_hdr *ip6h;
    struct tcphdr *tcph;
    struct ether_header *ethh;
    int packet_size;

    packet_size = SIZEOF_ETHER + SIZEOF_IPV6 + SIZEOF_TCP;

    ethh = (struct ether_header *) malloc_check( packet_size );
    ip6h = (struct ip6_hdr *) ( (char *) ethh + SIZEOF_ETHER );
    tcph = (struct tcphdr *) ( (char *) ip6h + SIZEOF_IPV6 );

    build_ethernet( ethh, interface, dstmac, ETHERTYPE_IPV6 );
    build_ipv6( ip6h, srcip, dstip, IPPROTO_TCP, SIZEOF_TCP );
    build_tcp_syn( ip6h, tcph, SOURCE_PORT, dstport );

    if ( pcap_inject( pcap, ethh, packet_size ) != packet_size ) errx( 1, "pcap_inject" );
    free( ethh );
}

void do_ipv6_short_tcp_frag( char *interface, char *srcip, char *dstip, char *dstmac, unsigned short dstport )
{
    struct ip6_hdr *ip6h;
    struct tcphdr *tcph;
    struct ether_header *ethh;
    int packet_size;
    unsigned short fragid = rand();

    packet_size = SIZEOF_ETHER + SIZEOF_IPV6 + sizeof( struct ip6_frag ) + MINIMUM_FRAGMENT_SIZE;

    ethh = (struct ether_header *) malloc_check( BIG_PACKET_SIZE );
    ip6h = (struct ip6_hdr *) ( (char *) ethh + SIZEOF_ETHER );
    tcph = (struct tcphdr *) ( (char *) ip6h + SIZEOF_IPV6 + sizeof( struct ip6_frag ) );

    build_ethernet( ethh, interface, dstmac, ETHERTYPE_IPV6 );
    build_ipv6_short_frag1( ip6h, srcip, dstip, IPPROTO_TCP, fragid );
    build_tcp_syn( ip6h, tcph, SOURCE_PORT, dstport );

    if ( pcap_inject( pcap, ethh, packet_size ) != packet_size ) errx( 1, "pcap_inject" );

    packet_size = SIZEOF_ETHER + SIZEOF_IPV6 + sizeof( struct ip6_frag ) + SIZEOF_TCP - MINIMUM_FRAGMENT_SIZE;

    build_ipv6_frag2( ip6h, srcip, dstip, IPPROTO_TCP, fragid, SIZEOF_TCP - MINIMUM_FRAGMENT_SIZE );
    memmove( tcph, (char *) tcph + MINIMUM_FRAGMENT_SIZE, SIZEOF_TCP - MINIMUM_FRAGMENT_SIZE );

    if ( pcap_inject( pcap, ethh, packet_size ) != packet_size ) errx( 1, "pcap_inject" );
    free( ethh );
}

void do_ipv6_short_icmp_frag( char *interface, char *srcip, char *dstip, char *dstmac )
{
    struct ip6_hdr *ip6h;
    struct icmp6_hdr *icmp6h;
    struct ether_header *ethh;
    int packet_size;
    unsigned short fragid = rand();
    unsigned short pinglen = 40;

    packet_size = SIZEOF_ETHER + SIZEOF_IPV6 + sizeof( struct ip6_frag ) + MINIMUM_FRAGMENT_SIZE;

    ethh = (struct ether_header *) malloc_check( BIG_PACKET_SIZE );
    ip6h = (struct ip6_hdr *) ( (char *) ethh + SIZEOF_ETHER );
    icmp6h = (struct icmp6_hdr *) ( (char *) ip6h + SIZEOF_IPV6 + sizeof( struct ip6_frag ) );

    build_ethernet( ethh, interface, dstmac, ETHERTYPE_IPV6 );
    build_ipv6_short_frag1( ip6h, srcip, dstip, IPPROTO_ICMPV6, fragid );
    build_icmp6_ping( ip6h, icmp6h, pinglen );

    if ( pcap_inject( pcap, ethh, packet_size ) != packet_size ) errx( 1, "pcap_inject" );

    packet_size = SIZEOF_ETHER + SIZEOF_IPV6 + sizeof( struct ip6_frag ) + SIZEOF_PING + pinglen - MINIMUM_FRAGMENT_SIZE;

    build_ipv6_frag2( ip6h, srcip, dstip, IPPROTO_ICMPV6, fragid, SIZEOF_TCP - MINIMUM_FRAGMENT_SIZE );
    memmove( icmp6h, (char *) icmp6h + MINIMUM_FRAGMENT_SIZE, SIZEOF_TCP - MINIMUM_FRAGMENT_SIZE );

    if ( pcap_inject( pcap, ethh, packet_size ) != packet_size ) errx( 1, "pcap_inject" );
    free( ethh );
}

void do_ipv6_optioned_icmp_frag( char *interface, char *srcip, char *dstip, char *dstmac )
{
    struct ip6_hdr *ip6h;
    struct icmp6_hdr *icmp6h, *icmp6h_optioned;
    struct ether_header *ethh;
    int packet_size;
    unsigned short fragid = rand();
    unsigned short optlen = fix_up_destination_options_length(
         MINIMUM_PACKET_SIZE - SIZEOF_IPV6 - sizeof( struct ip6_dest ) - sizeof( struct ip6_frag ) - MINIMUM_FRAGMENT_SIZE
    );
    /*
     * pinglen must be > 6 or our first packet will be <= MINIMUM_PACKET_SIZE bytes and
     * our second packet empty. 
     */
    unsigned short pinglen = 40;

    packet_size = SIZEOF_ETHER + SIZEOF_IPV6 + sizeof( struct ip6_dest ) + optlen + sizeof( struct ip6_frag ) + MINIMUM_FRAGMENT_SIZE;

    ethh = (struct ether_header *) malloc_check( BIG_PACKET_SIZE );
    ip6h = (struct ip6_hdr *) ( (char *) ethh + SIZEOF_ETHER );
    icmp6h = (struct icmp6_hdr *) ( (char *) ip6h + SIZEOF_IPV6 + sizeof( struct ip6_frag ) );
    icmp6h_optioned = (struct icmp6_hdr *) ( (char *) ip6h + SIZEOF_IPV6 + sizeof( struct ip6_dest ) + optlen + sizeof( struct ip6_frag ) );

    build_ethernet( ethh, interface, dstmac, ETHERTYPE_IPV6 );
    build_ipv6_optioned_frag1( ip6h, srcip, dstip, IPPROTO_ICMPV6, fragid, optlen );
    build_icmp6_ping( ip6h, icmp6h_optioned, pinglen );

    if ( pcap_inject( pcap, ethh, packet_size ) != packet_size ) errx( 1, "pcap_inject" );

    packet_size = SIZEOF_ETHER + SIZEOF_IPV6 + sizeof( struct ip6_frag ) + SIZEOF_PING + pinglen - MINIMUM_FRAGMENT_SIZE;

    build_ipv6_frag2( ip6h, srcip, dstip, IPPROTO_ICMPV6, fragid, SIZEOF_ICMP6 + pinglen - MINIMUM_FRAGMENT_SIZE );
    memmove( icmp6h, (char *) icmp6h_optioned + MINIMUM_FRAGMENT_SIZE, SIZEOF_ICMP6 + pinglen - MINIMUM_FRAGMENT_SIZE );

    if ( pcap_inject( pcap, ethh, packet_size ) != packet_size ) errx( 1, "pcap_inject" );
    free( ethh );
}

void do_ipv6_optioned_tcp_frag( char *interface, char *srcip, char *dstip, char *dstmac, unsigned short dstport )
{
    struct ip6_hdr *ip6h;
    struct tcphdr *tcph, *tcph_optioned;
    struct ether_header *ethh;
    int packet_size;
    unsigned short fragid = rand();
    unsigned short optlen = fix_up_destination_options_length(
        MINIMUM_PACKET_SIZE - SIZEOF_IPV6 - sizeof( struct ip6_dest ) - sizeof( struct ip6_frag ) - MINIMUM_FRAGMENT_SIZE
    );

    packet_size = SIZEOF_ETHER + SIZEOF_IPV6 + sizeof( struct ip6_dest ) + optlen + sizeof( struct ip6_frag ) + MINIMUM_FRAGMENT_SIZE;

    ethh = (struct ether_header *) malloc_check( BIG_PACKET_SIZE );
    ip6h = (struct ip6_hdr *) ( (char *) ethh + SIZEOF_ETHER );
    tcph = (struct tcphdr *) ( (char *) ip6h + SIZEOF_IPV6 + sizeof( struct ip6_frag ) );
    tcph_optioned = (struct tcphdr *) ( (char *) ip6h + SIZEOF_IPV6 + sizeof( struct ip6_dest ) + optlen + sizeof( struct ip6_frag ) );

    build_ethernet( ethh, interface, dstmac, ETHERTYPE_IPV6 );
    build_ipv6_optioned_frag1( ip6h, srcip, dstip, IPPROTO_TCP, fragid, optlen );
    build_tcp_syn( ip6h, tcph_optioned, SOURCE_PORT, dstport );

    if ( pcap_inject( pcap, ethh, packet_size ) != packet_size ) errx( 1, "pcap_inject" );

    packet_size = SIZEOF_ETHER + SIZEOF_IPV6 + sizeof( struct ip6_frag ) + SIZEOF_TCP - MINIMUM_FRAGMENT_SIZE;

    build_ipv6_frag2( ip6h, srcip, dstip, IPPROTO_TCP, fragid, SIZEOF_TCP - MINIMUM_FRAGMENT_SIZE );
    memmove( tcph, (char *) tcph_optioned + MINIMUM_FRAGMENT_SIZE, SIZEOF_TCP - MINIMUM_FRAGMENT_SIZE );

    if ( pcap_inject( pcap, ethh, packet_size ) != packet_size ) errx( 1, "pcap_inject" );
    free( ethh );
}

/* Process functions. */
void fork_pcap_listener( char *dstip, char *srcip, unsigned short dstport, unsigned short srcport, enum TEST_TYPE test_type, long receive_timeout )
{
    char *packet_buf;
    char buf;
    int r;

    if ( pipe(pfd) == -1 ) err( 1, "Failed to creatre pipe." );

    listener_pid = fork();
    if ( listener_pid == -1 ) err( 1, "Failed to fork" );
    if ( listener_pid )  {
        close( pfd[1] );
        read( pfd[0], &buf, 1 );
        return;
    }
    close( pfd[0] );
    /*
     * Due to how pcap works there's still a race between we signal that we are
     * ready and when we actually call select(). Will this even help with our
     * race condition if write() doesn't block? The documentation seems unclear
     * as to the advantages of O_NONBLOCK with a 1 byte write.
     */
    fcntl( pfd[1], F_SETFL, O_NONBLOCK );

    pcap_setdirection( pcap, PCAP_D_IN );

    r = receive_a_packet( dstip, srcip, dstport, SOURCE_PORT, test_type, &packet_buf, receive_timeout );
    if ( r ) {
        write( pfd[1], &r, sizeof( int ) );
        write( pfd[1], packet_buf, r );
    } else {
        r = 0;
        write( pfd[1], &r, sizeof( int ) );
    }
    close( pfd[1] );
    exit( 0 );
}

int harvest_pcap_listener( char **packet_buf ) {
    int packet_buf_size;

    if ( read( pfd[0], &packet_buf_size, sizeof( int ) ) < sizeof( int ) )
        errx( 1, "Error communicating with child process." );
    /* No packet received. */
    if ( packet_buf_size == 0 ) return 0;
    if ( packet_buf_size > PCAP_CAPTURE_LEN || packet_buf_size < 1 )
        errx( 1, "Bad data received from child process." );
    *packet_buf = (char *) malloc_check( packet_buf_size );
    if ( read( pfd[0], *packet_buf, packet_buf_size ) < packet_buf_size )
        errx( 1, "Error communicating with child process (2)." );
    wait( NULL );
    return packet_buf_size;
}

void print_test_types( void )
{
    char *test;
    int x = 0;

    fprintf( stderr, "Available test types:\n\n" );
    while ( ( test = test_names[x++] ) ) {
        fprintf( stderr, "%s\n", test );
    }
}

void exit_with_usage( void )
{
    fprintf( stderr, "synfrag usage:\n" );
    fprintf( stderr, "--help | -h  This message.\n" );
    fprintf( stderr, "--srcip      Source IP address (this hosts)\n" );
    fprintf( stderr, "--dstip      Destination IP address (target)\n" );
    /* Currently not used.
    fprintf( stderr, "--srcport    Source port for TCP tests\n" ); */
    fprintf( stderr, "--dstport    Destination port for TCP tests\n" );
    fprintf( stderr, "--dstmac     Destination MAC address (default gw or target host if on subnet)\n" );
    fprintf( stderr, "--interface  Packet source interface\n" );
    fprintf( stderr, "--test       Type of test to run\n" );
    fprintf( stderr, "--timeout    Reply timeout in seconds (defaults to 10)\n\n" );
    print_test_types();
    fprintf( stderr, "\nAll TCP tests send syn packets, all ICMP/6 test send ping.\n" );
    fprintf( stderr, "All \"frag\" tests send fragments that are below the minimum packet size.\n" );
    fprintf( stderr, "All \"optioned\" tests send fragments that meet the minimum packet size.\n" );
    exit( 2 );
}

void copy_arg_string( char **dst, char *opt )
{
    *dst = malloc( strlen( opt ) );
    memcpy( *dst, opt, strlen( opt) );
}

void ip_test_arg( char *opt )
{
    struct in6_addr iptest;
    if (
        ( !inet_pton( AF_INET, opt, &iptest ) ) &&
        ( !inet_pton( AF_INET6, opt, &iptest ) )
    ) errx( 1, "Invalid IP address: %s", opt );
}

enum TEST_TYPE parse_args(
    int argc,
    char **argv,
    char **srcip,
    char **dstip,
    unsigned short *srcport,
    unsigned short *dstport,
    char **dstmac,
    char **interface,
    char **test_name,
    long *timeout
) {
    int x = 0;
    int option_index = 0;
    int c, tmpport;
    long tmptime;
    char *possible_match;
    enum TEST_TYPE test_type = 0;
    static struct option long_options[] = {
        {"srcip", required_argument, 0, 0},
        {"dstip", required_argument, 0, 0},
        {"srcport", required_argument, 0, 0},
        {"dstport", required_argument, 0, 0},
        {"dstmac", required_argument, 0, 0},
        {"interface", required_argument, 0, 0},
        {"test", required_argument, 0, 0},
        {"help", no_argument, 0, 0},
        {"timeout", required_argument, 0, 0},
        {0, 0, 0, 0}
    };

    if ( argc < 2 ) exit_with_usage();

    *srcip = *dstip = *dstmac = *interface = NULL;
    *srcport = *dstport = 0;

    while ( 1 ) {
        c = getopt_long(argc, argv, "h", long_options, &option_index);
        if ( c != 0 && c != 'h' ) break;

        if ( ( c == 'h' ) || ( strcmp( long_options[option_index].name, "help" ) == 0 ) ) {
            exit_with_usage();

        } else if ( strcmp( long_options[option_index].name, "srcip" ) == 0 ) {
            copy_arg_string( srcip, optarg );

        } else if ( strcmp( long_options[option_index].name, "dstip" ) == 0 ) {
            copy_arg_string( dstip, optarg );

        } else if ( strcmp( long_options[option_index].name, "dstmac" ) == 0 ) {
            copy_arg_string( dstmac, optarg );

        } else if ( strcmp( long_options[option_index].name, "interface" ) == 0 ) {
            copy_arg_string( interface, optarg );

        } else if ( strcmp( long_options[option_index].name, "srcport" ) == 0 ) {
            tmpport = atoi( optarg );
            if ( tmpport > 65535 || tmpport < 1 ) errx( 1, "Invalid value for srcport" );
            *srcport = (unsigned short) tmpport;

        } else if ( strcmp( long_options[option_index].name, "dstport" ) == 0 ) {
            tmpport = atoi( optarg );
            if ( tmpport > 65535 || tmpport < 1 ) errx( 1, "Invalid value for dstport" );
            *dstport = (unsigned short) tmpport;

        } else if ( strcmp( long_options[option_index].name, "timeout" ) == 0 ) {
            tmptime = atol( optarg );
            if ( tmptime < 1 ) errx( 1, "Invalid value for timeout" );
            *timeout = tmptime;

        } else if ( strcmp( long_options[option_index].name, "test" ) == 0 ) {
            while ( ( possible_match = test_names[x] ) ) {
                if ( strcmp( optarg, possible_match ) == 0 ) {
                    test_type = test_indexes[x];
                    *test_name = test_names[x];
                    break;
                }
                x++;
            }
        }
    }

    if ( optind < argc ) exit_with_usage();

    if ( !*srcip ) errx( 1, "Missing srcip" );
    if ( !*dstip ) errx( 1, "Missing dstip" );
    if ( !*dstmac ) errx( 1, "Missing dstmac" );
    if ( !*interface ) errx( 1, "Missing interface" );
    if ( !test_type ) {
        fprintf( stderr, "Missing or invalid test type.\n" );
        print_test_types();
        exit( 1 );
    }

    if ( IS_TEST_TCP( test_type ) ) {
        /* Currently not used.
        if ( !*srcport ) errx( 1, "Missing srcport" ); */
        if ( !*dstport ) errx( 1, "Missing dstport" );
    }

    return test_type;
}

int main( int argc, char **argv )
{
    char pcaperr[PCAP_ERRBUF_SIZE];
    int r;
    enum TEST_TYPE test_type;
    char *interface;
    char *srcip;
    char *dstip;
    char *dstmac;
    unsigned short dstport;
    unsigned short srcport;
    char *packet_buf;
    char *test_name;
    long receive_timeout = DEFAULT_TIMEOUT_SECONDS;

    test_type = parse_args( argc, argv, &srcip, &dstip, &srcport, &dstport, &dstmac, &interface, &test_name, &receive_timeout );
    srand( getpid() );

    printf( "Starting test \"%s\". Opening interface \"%s\".\n\n", test_name, interface );
    if ( ( pcap = pcap_open_live( interface, PCAP_CAPTURE_LEN, 0, 1, pcaperr ) ) == NULL )
        errx( 1, "pcap_open_live failed: %s", pcaperr );

    if ( pcap_datalink( pcap ) != DLT_EN10MB )
        errx( 1, "non-ethernet interface specified." );

    fork_pcap_listener( dstip, srcip, dstport, SOURCE_PORT, test_type, receive_timeout );

    switch ( test_type ) {
        case TEST_IPV4_TCP:
            do_ipv4_syn( interface, srcip, dstip, dstmac, dstport );
            break;
        case TEST_FRAG_IPV4_TCP:
            do_ipv4_short_tcp_frag( interface, srcip, dstip, dstmac, dstport );
            break;
        case TEST_FRAG_IPV4_ICMP:
            do_ipv4_short_icmp_frag( interface, srcip, dstip, dstmac );
            break;
        case TEST_FRAG_OPTIONED_IPV4_TCP:
            do_ipv4_optioned_tcp_frag( interface, srcip, dstip, dstmac, dstport );
            break;
        case TEST_FRAG_OPTIONED_IPV4_ICMP:
            do_ipv4_optioned_icmp_frag( interface, srcip, dstip, dstmac );
            break;

        case TEST_IPV6_TCP:
            do_ipv6_syn( interface, srcip, dstip, dstmac, dstport );
            break;
        case TEST_FRAG_IPV6_TCP:
            do_ipv6_short_tcp_frag( interface, srcip, dstip, dstmac, dstport );
            break;
        case TEST_FRAG_IPV6_ICMP6:
            do_ipv6_short_icmp_frag( interface, srcip, dstip, dstmac );
            break;
        case TEST_FRAG_OPTIONED_IPV6_TCP:
            do_ipv6_optioned_tcp_frag( interface, srcip, dstip, dstmac, dstport );
            break;
        case TEST_FRAG_OPTIONED_IPV6_ICMP6:
            do_ipv6_optioned_icmp_frag( interface, srcip, dstip, dstmac );
            break;

        default:
            errx( 1, "Unsupported test type!" );
    }

    printf( "Packet transmission successful, waiting for reply...\n\n" );

    r = harvest_pcap_listener( &packet_buf );
    if ( !r ) {
        fprintf( stderr, "Test failed, no response before time out (%li seconds).\n", receive_timeout );
        return 1;
    }
    if ( check_received_packet( r, packet_buf, test_type ) ) {
        printf( "Test was successful.\n" );
        free( packet_buf );
        return 0;
    }
    fprintf( stderr, "Test failed.\n" );
    free( packet_buf );
    return 1;
}

