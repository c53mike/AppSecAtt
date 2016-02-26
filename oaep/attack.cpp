#include <iostream>
#include "attack.h"

using namespace std;

pid_t pid = 0;    // process ID (of either parent or child) from fork

int target_raw[2];   // unbuffered communication: attacker -> attack target
int attack_raw[2];   // unbuffered communication: attack target -> attacker

FILE* target_out = NULL; // buffered attack target input  stream
FILE* target_in  = NULL; // buffered attack target output stream

void attack(char* argv2);
void cleanup(int s);

int main(int argc, char* argv[])
{

	// Ensure we clean-up correctly if Control-C (or similar) is signalled.
  	signal(SIGINT, &cleanup);

	// Create pipes to/from attack target; if it fails the reason is stored
	// in errno, but we'll just abort.
	if(pipe(target_raw) == -1)
		abort();
	  
	if(pipe(attack_raw) == -1)
		abort();

	switch(pid = fork()) 
	{ 
	    case -1: 
			// The fork failed; reason is stored in errno, but we'll just abort.
			abort();

	    case +0: 
	    {
			// (Re)connect standard input and output to pipes.
			close(STDOUT_FILENO);
			if(dup2(attack_raw[1], STDOUT_FILENO) == -1)
				abort();

			close(STDIN_FILENO);
			if(dup2(target_raw[0], STDIN_FILENO) == -1)
				abort();

			// Produce a sub-process representing the attack target.
			execl(argv[1], argv[0], NULL);

			// Break and clean-up once finished.
			break;
	    }

	    default:
	    {
			// Construct handles to attack target standard input and output.
			if((target_out = fdopen(attack_raw[0], "r")) == NULL) 
				abort();

			if((target_in = fdopen(target_raw[1], "w")) == NULL)
				abort();

			// Execute a function representing the attacker.
			attack(argv[2]);

			// Break and clean-up once finished.
			break;
	    }
	}

	// Clean up any resources we've hung on to.
	cleanup(SIGINT);

	return 0;
}


int interact(const mpz_class &l_prime, const mpz_class &c_prime)
{
    //cout << "In interact" << endl;
    // interact with 61061.D
	gmp_fprintf(target_in, "%ZX\n%0256ZX\n", l_prime.get_mpz_t(), c_prime.get_mpz_t());
    //cout << "Before flush" << endl;
	fflush(target_in);
    //cout << "After flush" << endl;
    
    //       code 0: decryption success 
    // error code 1: y >= B
    // error code 2: y < B (?)
    
    // Print error code
	int code;
	fscanf(target_out, "%X", &code);
	//cout << "Error code: " << code << "\n";
    return code;
}

void attack(char* argv2)
{
	// interact with 61061.conf
    // reading the input
	ifstream config (argv2, ifstream::in);
	mpz_class N, e, l_prime, c_prime;
	config >> hex >> N >> e >> l_prime >> c_prime;
    
    // print k = ceil(log 256 (N))
	//size_t sizeN = mpz_size(N.get_mpz_t());
    size_t k = mpz_size(N.get_mpz_t()) * mp_bits_per_limb / 8;
	//cout << "size of N in bytes: " << k << "\n";
    // print B = 2^(8*(k-1)) (mod N)
    // !!! assuming 2*B < N !!!
    mpz_class B;
    mpz_powm_ui(B.get_mpz_t(), mpz_class(2).get_mpz_t(), 8*(k - 1), N.get_mpz_t());
    //cout << "B = " << B << "\n";
    
    //////////////////////////////////////////////////////////////////////
    // ATTACK                                                           //
    //////////////////////////////////////////////////////////////////////
    
    //////////////////////////////////////////////////////////////////////
    // STEP 1.
    int code = -1, i = 1;
    mpz_class f_1;
    mpz_class f_1_exp;
    mpz_class c_1; // c_1 = f_1 * c' (mod N)

    while (code != 1) 
    {
        mpz_ui_pow_ui(f_1.get_mpz_t(), 2, i);
        mpz_powm(f_1_exp.get_mpz_t(), f_1.get_mpz_t(), e.get_mpz_t(), N.get_mpz_t());
        c_1 = f_1 * c_prime % N;
        code = interact(l_prime, c_1);
        i++;
    }
    
    //cout << "f_1 c [B/2, 2*B) = " << f_1 << "\n";
    
    
    //////////////////////////////////////////////////////////////////////
    // STEP 2.
    
    // f_2 = 2*B/f_1
	mpz_class f_2 = (N + B) / B * f_1 / 2;
    //cout << "f_2 = " << f_2 << "\n";
    mpz_class f_2_exp;
    mpz_class c_2;
    code = -1;
    
    while (true)
    {
        mpz_powm(f_2_exp.get_mpz_t(), f_2.get_mpz_t(), e.get_mpz_t(), N.get_mpz_t());
        c_2 = f_2_exp * c_prime % N;        
        code = interact(l_prime, c_2);
        
        if (code != 1)
            break;
        
        f_2 += f_1/2;
    }
    
    //cout << "f_2 = " << f_2 << "\n";
    
    //////////////////////////////////////////////////////////////////////
    // STEP 3.
    
    // m_min = ceil( n / f_2 )
    mpz_class m_min = (N + f_2 - 1)/f_2;
    //cout << "m_min = " << m_min << endl;
    // m_max = floor( (n + B) / f_2 )
    mpz_class m_max = (N + B)/f_2;
    //cout << "m_max = " << m_max << endl;
    
    mpz_class f_3, f_3_exp, c_3, f_tmp;
    mpz_class i_bound;
    
    while(m_min != m_max)
    {
        f_tmp = 2*B / (m_max - m_min);
        //cout << "f_tmp = " << f_tmp << "\n";
        
        i_bound = f_tmp * m_min / N;
        f_3 = (i_bound * N + m_min - 1) / m_min;
        
        mpz_powm(f_3_exp.get_mpz_t(), f_3.get_mpz_t(), e.get_mpz_t(), N.get_mpz_t());
        c_3 = f_3_exp * c_prime % N;
        
        code = interact(l_prime, c_3);
        
        if (code == 1)
            m_min = (i_bound * N + B + f_3 - 1) / f_3;
        else if (code == 2)
            m_max = (i_bound * N + B) / f_3;
    }
    
    mpz_class c_check;
    mpz_powm(c_check.get_mpz_t(), m_max.get_mpz_t(), e.get_mpz_t(), N.get_mpz_t());
    
    if (c_check == c_prime)
        cout << "MOO WINS!!!" << endl;
    
    
    size_t sizeinbase = mpz_sizeinbase(m_min.get_mpz_t(), 256); 
    //holder for the byte array
    unsigned char buffer[128] = {0}, bufferL[128] = {0};
    
    // convert m_min from mpz_class to a byte array
    // have the behaviour of I2OSP
    mpz_export(buffer + 128 - sizeinbase, NULL, 1, 1, 0, 0, m_min.get_mpz_t());
    
    cout << "Buffer = ";
    for (int j = 0; j < 128; j++)
        printf("%02X", (unsigned int)buffer[j]);
    
    cout << endl;
    
    //////////////////////////////////////////////////////////////////////
    // EME-OAEP Decoding                                                //
    //////////////////////////////////////////////////////////////////////
    
    // 3. a.
    // convert l_prime to byte array
    sizeinbase = mpz_sizeinbase(l_prime.get_mpz_t(), 256);
    mpz_export(bufferL, NULL, 1, 1, 0, 0, l_prime.get_mpz_t());
    
    // digest for l_prime
    unsigned char digest[SHA_DIGEST_LENGTH];
    
    size_t hLen = SHA_DIGEST_LENGTH;
    
    // hash
    SHA1(bufferL, sizeinbase, digest);
    
    cout << "lHash = ";
    for (int j = 0; j < SHA_DIGEST_LENGTH; j++)
        printf("%02X", (unsigned int)digest[j]);
    
    cout << endl;
    
    // 3. b.
    unsigned char Y = buffer[0];
    printf("Y = %02X", Y);
    cout << endl;
    
    cout << "maskedSeed = ";
    unsigned char maskedSeed[hLen];
    for (int j = 0; j < hLen; j++)
        maskedSeed[j] = buffer[j+1];
    for (int j = 0; j < hLen; j++)
        printf("%02X", (unsigned int)maskedSeed[j]);
    cout << endl;
    
    cout << "maskedDB = ";
    unsigned char maskedDB[k - hLen - 1];
    for (int j = 0; j < k - hLen - 1; j++)
        maskedDB[j] = buffer[j+hLen+1];
    for (int j = 0; j < k - hLen - 1; j++)
        printf("%02X", (unsigned int)maskedDB[j]);
    cout << endl;
    
    // 3. c.
    cout << "seedMask =   ";
    unsigned char seedMask[hLen];
    PKCS1_MGF1(seedMask, hLen, maskedDB, k - hLen - 1, EVP_sha1());
    for (int j = 0; j < hLen; j++)
        printf("%02X", seedMask[j]);
    cout << endl;
    
    // 3. d.
    cout << "seed = ";
    unsigned char seed[hLen];

    {
        int j = 0, l = 0, r = 0;
        
        for (; j < hLen; j++)
            if (maskedSeed[j] != 0)
                break;
        for (; l < hLen; l++)
            if (seedMask[l] != 0)
                break;
            
        if (j > l)
            l = j;
        
        for (; r < hLen && l < hLen; r++, l++)
            seed[r] = maskedSeed[l] ^ seedMask[l];

        for (r = 0; r < hLen; r++)
            printf("%02X", (unsigned int)seed[r]);
        cout << endl;
    }
    
    // 3. e.
    cout << "dbMask = ";
    unsigned char dbMask[k - hLen - 1];
    PKCS1_MGF1(dbMask, k - hLen - 1, seed, hLen, EVP_sha1());
    for (int j = 0; j < k - hLen - 1; j++)
        printf("%02X", (unsigned int)dbMask[j]);
    cout << endl;
    
    // 3. f.
    cout << "DB = ";
    unsigned char DB[k - hLen - 1];
    {
        int j = 0, l = 0, r = 0;
        
        for (; j < k - hLen - 1; j++)
            if (maskedDB[j] != 0)
                break;
        for (; l < k - hLen - 1; l++)
            if (dbMask[l] != 0)
                break;
            
        if (j > l)
            l = j;
        
        for (; r < k - hLen - 1 && l < k - hLen - 1; r++, l++)      
            DB[r] = maskedDB[l] ^ dbMask[l];
        for (int j = 0; j < r; j++)
            printf("%02X", (unsigned int)DB[j]);
        cout << endl;
        cout << endl;    
    }
    
    // 3. g.
    //cout << "lHash_prime = ";
    cout << "     ";
    unsigned char lHash_prime[hLen];
    for (int j = 0; j < hLen; j++)
        lHash_prime[j] = DB[j];
    for (int j = 0; j < hLen; j++)
        printf("%02X", DB[j]);
}


void cleanup(int s) 
{
	// Close the   buffered communication handles.
	fclose(target_in);
	fclose(target_out);

	// Close the unbuffered communication handles.
	close(target_raw[0]); 
	close(target_raw[1]); 
	close(attack_raw[0]); 
	close(attack_raw[1]); 

	// Forcibly terminate the attack target process.
	if( pid > 0 )
		kill(pid, SIGKILL);

	// Forcibly terminate the attacker process.
	exit(1); 
}
