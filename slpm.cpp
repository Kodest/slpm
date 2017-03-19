#include "buffer.h"
#include "fd.h"
#include "utils.h"

#include <sodium/crypto_auth_hmacsha256.h>
#include <sodium/crypto_pwhash_scryptsalsa208sha256.h>
#include <sodium/crypto_sign_ed25519.h>

#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <cstring>
#include <cassert>
#include <sys/un.h>
#include <experimental/optional>
#include <algorithm>

#include "ssh-agent.h" // TODO: move up

#define COUNT(x) (sizeof(x) / sizeof(x[0]))

static const char*
lookup_pass_chars(char templat)
{
	switch (templat) {
	case 'V': return "AEIOU";
	case 'C': return "BCDFGHJKLMNPQRSTVWXYZ";
	case 'v': return "aeiou";
	case 'c': return "bcdfghjklmnpqrstvwxyz";
	case 'A': return "AEIOUBCDFGHJKLMNPQRSTVWXYZ";
	case 'a': return "AEIOUaeiouBCDFGHJKLMNPQRSTVWXYZbcdfghjklmnpqrstvwxyz";
	case 'n': return "0123456789";
	case 'o': return "@&%?,=[]_:-+*$#!'^~;()/.";
	case 'x': return "AEIOUaeiouBCDFGHJKLMNPQRSTVWXYZbcdfghjklmnpqrstvwxyz0123456789!@#$%^&*()";
	}
	assert(!"invalid template");
	return 0;
}

static const char* temp_max_sec[] = {
	  "anoxxxxxxxxxxxxxxxxx"
	, "axxxxxxxxxxxxxxxxxno"
};

static const char* temp_long[] = {
	  "CvcvnoCvcvCvcv"
	, "CvcvCvcvnoCvcv"
	, "CvcvCvcvCvcvno"
	, "CvccnoCvcvCvcv"
	, "CvccCvcvnoCvcv"
	, "CvccCvcvCvcvno"
	, "CvcvnoCvccCvcv"
	, "CvcvCvccnoCvcv"
	, "CvcvCvccCvcvno"
	, "CvcvnoCvcvCvcc"
	, "CvcvCvcvnoCvcc"
	, "CvcvCvcvCvccno"
	, "CvccnoCvccCvcv"
	, "CvccCvccnoCvcv"
	, "CvccCvccCvcvno"
	, "CvcvnoCvccCvcc"
	, "CvcvCvccnoCvcc"
	, "CvcvCvccCvccno"
	, "CvccnoCvcvCvcc"
	, "CvccCvcvnoCvcc"
	, "CvccCvcvCvccno"
};

static const char* temp_medium[] = {
	  "CvcnoCvc"
	, "CvcCvcno"
};

static const char* temp_short[] = {
	  "Cvcn"
};

static const char* temp_basic[] = {
	  "aaanaaan"
	, "aannaaan"
	, "aaannaaa"
};

static const char* temp_pin[] = {
	  "nnnn"
};

#define DEF_TEMP(name, t) { name, t, COUNT(t) }

static const struct {
	const char* name;
	const char** templat;
	unsigned count;
} templates[] = {
	  DEF_TEMP("Maximum Security Password", temp_max_sec)
	, DEF_TEMP("Long Password", temp_long)
	, DEF_TEMP("Medium Password", temp_medium)
	, DEF_TEMP("Short Password", temp_short)
	, DEF_TEMP("Basic Password", temp_basic)
	, DEF_TEMP("PIN", temp_pin)
};

static int
hmacsha256(
	  uint8_t *out
	, const uint8_t *in, size_t inlen
	, const uint8_t *k, size_t klen
)
{
	struct crypto_auth_hmacsha256_state state;
	if (crypto_auth_hmacsha256_init(&state, k, klen)) return -1;
	if (crypto_auth_hmacsha256_update(&state, in, inlen)) return -2;
	if (crypto_auth_hmacsha256_final(&state, out)) return -3;
	return 0;
}

using Seed = std::array<uint8_t, crypto_auth_hmacsha256_BYTES>;

static void
output_site_generic(const Seed& seed)
{
	Buffer<uint8_t, 4096> buf;
	for (unsigned i = 0; i != COUNT(templates); ++i) {
		buf += templates[i].name;
		buf += ": ";
		const char* templat = templates[i].templat[seed[0] % templates[i].count];
		for (unsigned j = 0; templat[j]; ++j) {
			const char* pass_chars = lookup_pass_chars(templat[j]);
			int len = strlen(pass_chars);
			assert(seed.size() > 1 + j);
			buf += pass_chars[seed[1 + j] % len];
		}
		buf += '\n';
	}

	buf.write(STDOUT_FILENO);
}

static void
append(Buffer<uint8_t, 4096>& result, const Ed25519PublicKey& pk)
{
	Buffer<char, 256> buf;
	buf.append_with_be32_length_prefix("ssh-ed25519");
	buf.append_with_be32_length_prefix(reinterpret_cast<const char*>(pk.data()), pk.size());

	std::array<char, 256> base64;
	to_base64(base64.data(), base64.size(), buf.data(), buf.size());

	result += base64.data();
}

static void
output_site_ssh(SshAgent& sa, const Seed& seed, const char* site)
{
	assert(seed.size() >= crypto_sign_ed25519_SEEDBYTES);
	Ed25519KeyPair k;
	crypto_sign_ed25519_seed_keypair(k.pub.data(), k.sec.data(), seed.data());
	Buffer<char, 256> comment;
	comment += "slpm+";
	comment += site;
	comment += '\0';
	const auto error = sa.add(k, comment.data());
	sodium_memzero(k.sec.data(), k.sec.size());

	if (!error) {
		Buffer<uint8_t, 4096> buf;
		buf += "ssh-ed25519";
		buf += ' ';
		append(buf, k.pub);
		buf += ' ';
		const auto* user = getenv("USER");
		buf += user ? user : "user";
		buf += '@';
		buf += "slpm+";
		buf += site;
		buf += '\n';
		buf.write(STDOUT_FILENO);
	}
	sodium_memzero(k.pub.data(), k.pub.size());
}

static const char iv[] = "com.lyndir.masterpassword";

static void
write_passwords_for_site(SshAgent& sa, const uint8_t* key, size_t keysize, const char* site, int counter)
{
	const auto is_ssh = !strncmp(site, "ssh ", 4);
	if (is_ssh) site += 4;
	Buffer<uint8_t, 4096> buf;

	buf += iv;
	buf.append_with_be32_length_prefix(site);
	buf.append_network_long(counter);
	Seed seed;
	if (hmacsha256(seed.data(), buf.data(), buf.size(), key, keysize)) {
		writes(2, "hmac fail\n");
		return;
	}

	if (is_ssh) {
		output_site_ssh(sa, seed, site);
	} else {
		output_site_generic(seed);
	}
	sodium_memzero(seed.data(), seed.size());
}

static char*
mygetstring(const char* prompt, int infd = STDIN_FILENO, int outfd = STDOUT_FILENO)
{
	static char buffer[256];
	static int sord = 0;
	static int processed = 0;
	
	writes(outfd, prompt);

	if (processed) {
		sodium_memzero(buffer, processed);
		sord -= processed;
		memcpy(buffer, buffer + processed, sord);
		processed = 0;
	}

	while (!0) {
		if (char *const eoln = reinterpret_cast<char*>(memchr(buffer, '\n', sord))) {
			*eoln = '\0';
			processed = eoln + 1 - buffer;
			return buffer;
		}
		const ssize_t rd = read(infd, buffer + sord, sizeof(buffer) - sord);
		if (rd <= 0) return 0;
		sord += rd;
	}
}

static char* getstring(const char* prompt) { return mygetstring(prompt); }

struct HiddenInput {
	~HiddenInput()
	{
		ioctl(fd_.get(), TCSETSF, &s_);
		writes(fd_.get(), "\n");
	}

	HiddenInput()
	: fd_(open("/dev/tty", O_RDWR | O_NOCTTY | O_CLOEXEC))
	{
		struct termios t;
		ioctl(fd_.get(), TCGETS, &t);
		s_ = t;
		t.c_lflag &= ~(ECHO|ISIG);
		t.c_lflag |= ICANON;
		t.c_iflag &= ~(INLCR|IGNCR);
		t.c_iflag |= ICRNL;
		ioctl(fd_.get(), TCSETSF, &t);
		ioctl(fd_.get(), TCSBRK, !0);
	}

	HiddenInput(const HiddenInput&) = delete;
	HiddenInput& operator=(const HiddenInput&) = delete;

	char*
	getpass(const char* prompt)
	const
	{
		return mygetstring(prompt, fd_.get(), fd_.get());
	}

private:
	struct termios s_;
	Fd fd_;
};

static char*
mygetpass(const char* prompt)
{
	return HiddenInput().getpass(prompt);
}

void
sodium_memzero(void * const pnt, const size_t len)
{
	volatile unsigned char *volatile pnt_ =
		(volatile unsigned char * volatile)pnt;
	size_t i = (size_t) 0U;

	while (i < len) pnt_[i++] = 0U;
}

int
sodium_memcmp(const void * const b1_, const void * const b2_, size_t len)
{
	const volatile unsigned char *volatile b1 = (const volatile unsigned char * volatile) b1_;
	const volatile unsigned char *volatile b2 = (const volatile unsigned char * volatile) b2_;
	size_t i;
	unsigned char d = (unsigned char) 0U;

	for (i = 0U; i < len; i++) {
		d |= b1[i] ^ b2[i];
	}
	return (1 & ((d - 1) >> 8)) - 1;
}

int
main(int, char* [], char* envp[])
{
	environ = envp;
	const char* salt = getenv("SLPM_FULLNAME");
	if (!salt) salt = "";
	{
		Buffer<uint8_t, 256> buf;
		buf += "SLPM_FULLNAME='";
		buf += salt;
		buf += "'\n";
		buf.write(1);
	}

	Buffer<uint8_t, 4096> buf;
	buf += iv;
	buf.append_with_be32_length_prefix(salt);

	char *const pw = (isatty(STDIN_FILENO) ? mygetpass : getstring)("Passphrase: ");
	if (!pw) {
		writes(STDOUT_FILENO, "\n");
		return -1;
	}
	writes(1, "Deriving key...");
	uint8_t key[64];
	if (crypto_pwhash_scryptsalsa208sha256_ll(
		  (const uint8_t*)pw
		, strlen(pw)
		, buf.data()
		, buf.size()
		, 32768
		, 8
		, 2
		, key
		, sizeof(key)
	)) {
		sodium_memzero(pw, strlen(pw));
		writes(2, "scrypt fail\n");
		return -1;
	}
	sodium_memzero(pw, strlen(pw));

	writes(1, "\rKey derivation complete.\n");
	SshAgent sa;
	while (!0) {
		char site[256];
		const char* s = getstring("Site: ");
		if (!s) break;
		strncpy(site, s, sizeof(site) - 1);
		const char* c = getstring("Counter: ");
		if (!c) break;
		write_passwords_for_site(sa, key, sizeof(key), site, atoi(c));
	}

	sodium_memzero(key, sizeof(key));
	writes(1, "\rBye!    \n");
	return 0;
}
