#include "include/headers.h"
#include "include/maps.h"
#include "include/log.h"
#include "include/logger_messages.h"
#include "include/pids.h"
#include "include/common.h"

static __always_inline void tcp_kprobe(struct pt_regs *ctx, struct bpf_map_def *map_fd, _Bool is_send) {
	long err;

	__u64 id = bpf_get_current_pid_tgid();
	__u32 pid = id >> 32;

	// Get fd (from syscall read/write). Use it to flip a bit flag in file_descriptor_to_ipv4
	// Why? For an unknown reason we don't see debug prints here from Go programs, only from openssl.
	// However, we do know that the tcp_sendmsg and tcp_recvmsg are used by Go. We saw it with perf and with bpftrace.
	// Trying to understand if the debug prints are a false negative by making some effect here and printing it
	// somewhere else. We set a bit in fdinfo.flags and print it in output_ssl_chunk.
	__u32 *fd = bpf_map_lookup_elem(&pid_tgid_to_fd, &id);
	if (fd != NULL) {
		__u64 key = (__u64) pid << 32 | *fd;
		struct fd_info *fdinfo = bpf_map_lookup_elem(&file_descriptor_to_ipv4, &key);
		if (fdinfo != NULL) {
			fdinfo->flags = fdinfo->flags | (1 << 2);
		}
	}

	if (!should_tap(id >> 32)) {
		return;
	}

	struct ssl_info *info_ptr = bpf_map_lookup_elem(map_fd, &id);
	// Happens when the connection is not tls
	if (info_ptr == NULL) {
		return;
	}

	struct sock *sk = (struct sock *) PT_REGS_PARM1(ctx);

	short unsigned int family;
	err = bpf_probe_read(&family, sizeof(family), (void *)&sk->__sk_common.skc_family);
	if (err != 0) {
		log_error(ctx, LOG_ERROR_READING_SOCKET_FAMILY, id, err, 0l);
		return;
	}
	if (family != AF_INET) {
		return;
	}

	// daddr, saddr and dport are in network byte order (big endian)
	// sport is in host byte order
	__be32 saddr;
	__be32 daddr;
	__be16 dport;
	__u16 sport;

	err = bpf_probe_read(&saddr, sizeof(saddr), (void *)&sk->__sk_common.skc_rcv_saddr);
	if (err != 0) {
		log_error(ctx, LOG_ERROR_READING_SOCKET_SADDR, id, err, 0l);
		return;
	}
	err = bpf_probe_read(&daddr, sizeof(daddr), (void *)&sk->__sk_common.skc_daddr);
	if (err != 0) {
		log_error(ctx, LOG_ERROR_READING_SOCKET_DADDR, id, err, 0l);
		return;
	}
	err = bpf_probe_read(&dport, sizeof(dport), (void *)&sk->__sk_common.skc_dport);
	if (err != 0) {
		log_error(ctx, LOG_ERROR_READING_SOCKET_DPORT, id, err, 0l);
		return;
	}
	err = bpf_probe_read(&sport, sizeof(sport), (void *)&sk->__sk_common.skc_num);
	if (err != 0) {
		log_error(ctx, LOG_ERROR_READING_SOCKET_SPORT, id, err, 0l);
		return;
	}

	info_ptr->address_info.mode = ADDRESS_INFO_MODE_PAIR;
	info_ptr->address_info.daddr = daddr;
	info_ptr->address_info.saddr = saddr;
	info_ptr->address_info.dport = dport;
	info_ptr->address_info.sport = bpf_htons(sport);
}

SEC("kprobe/tcp_sendmsg")
void BPF_KPROBE(tcp_sendmsg) {
	__u64 id = bpf_get_current_pid_tgid();
	log_error(ctx, LOG_DEBUG, 2, 1, id);

	tcp_kprobe(ctx, &openssl_write_context, true);
}

SEC("kprobe/tcp_recvmsg")
void BPF_KPROBE(tcp_recvmsg) {
	__u64 id = bpf_get_current_pid_tgid();
	log_error(ctx, LOG_DEBUG, 2, 2, id);

	tcp_kprobe(ctx, &openssl_read_context, false);
}
