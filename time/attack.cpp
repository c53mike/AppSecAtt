#include <iostream>
#include "attack.h"

using namespace std;

pid_t pid = 0;    // process ID (of either parent or child) from fork

int target_raw[2];   // unbuffered communication: attacker -> attack target
int attack_raw[2];   // unbuffered communication: attack target -> attacker

FILE* target_out = NULL; // buffered attack target input  stream
FILE* target_in  = NULL; // buffered attack target output stream

struct bucket {
    mpz_class ciphertext;
    int time_mul;
    int time_red;
};

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

// Montgomery multiplication
mpz_class montgomery_multiplication(mpz_class x, mpz_class y, mp_limb_t omega, mpz_class N) 
{
    // work with a temp var instead of rop, so that the same variable can be passed as x and rop (similarly to native GMP functions)
    mpz_class r = 0;
    mp_limb_t u, y_i, x_0, r_0;
    
    // l_N - mpz_size(N)
    for (mp_size_t i = 0; i < mpz_size(N.get_mpz_t()); i++) 
    {
        // u <- (r_0 + y_i*x_0)*omega (mod b)
        y_i = mpz_getlimbn(y.get_mpz_t(), i); // i-th limb of y
        x_0 = mpz_getlimbn(x.get_mpz_t(), 0); // 0-th limb of x
        r_0 = mpz_getlimbn(r.get_mpz_t(), 0); // 0-th limb of r
        u = (r_0 + y_i * x_0) * omega;
        
        // r <- (r + y_i*x + u*N)/b
        r += y_i * x; // r <- r + y_i*x
        r += u * N;   // r <- r + u*N
        mpz_tdiv_q_2exp(r.get_mpz_t(), r.get_mpz_t(), mp_bits_per_limb);  // r <- r/b
    }
     
    return r; // mpz_swap is O(1), while mpz_set is O(n) where n is the number of limbs
}

// Computing omega
void montgomery_omega(mp_limb_t &omega, mpz_class N) 
{
    // omega <- 1 (mod b)
    omega = 1;
    
    // b is the 0th limb of N, 
    mp_limb_t b = mpz_getlimbn(N.get_mpz_t(), 0);
    
    for (mp_size_t i = 1; i <= mp_bits_per_limb; i++)
        // omega <- omega * omega * N (mod b)
        omega *= omega * b;
    
    // omega = -omega (mod b)
    omega = -omega;
}

// Computing rho^2
void montgomery_rho_sq(mpz_class &rho_sq, mpz_class N) 
{
    // rho_sq <- 1 (mod N)
    rho_sq = 1;
    
    // upto 2 * l_N * w
    for (mp_size_t i = 1; i < 2 * mpz_size(N.get_mpz_t()) * mp_bits_per_limb + 1; i++) 
    {
        // rho^2 <- rho^2 + rho^2
        rho_sq += rho_sq;
        
        // modular reduction instead of mpz_mod
        // if rho^2 > N, rho^2 <- rho^2 - N
        if (rho_sq > N)
            rho_sq -= N;
    }
}

// Convert a number into a montgomery number
// num should be < N
mpz_class montgomery_number(mpz_class num, mpz_class rho_sq, mp_limb_t omega, mpz_class N) 
{
    // r <- mont_num = num * rho (mod N)
    return montgomery_multiplication(num, rho_sq, omega, N);
}

// Montgomery reduction
void montgomery_reduction(mpz_class &rop, mpz_class t, mp_limb_t omega, mpz_class N) 
{
    // r <- t
    mpz_class r = t;
    mpz_class b_times_N;
    mp_limb_t u, r_i;
    
    // l_N - mpz_size
    for (mp_size_t i = 0; i < mpz_size(N.get_mpz_t()); i++) 
    {
        r_i = mpz_getlimbn(r.get_mpz_t(),i);
 
        // u <- r_i*omega (mod b)
        u = r_i * omega;
        
        // r <- r + (u*N*(b^i))
        mpz_mul_2exp(b_times_N.get_mpz_t(), N.get_mpz_t(), mp_bits_per_limb * i);
        r += b_times_N*u;
    }

    // r <- r / b ^ (l_N)
    mpz_tdiv_q_2exp(r.get_mpz_t(), r.get_mpz_t(), mp_bits_per_limb * mpz_size(N.get_mpz_t()) );
    
    // if r > N, r <- r - N
    if(r > N)
        r -= N;
    
    mpz_swap(rop.get_mpz_t(), r.get_mpz_t()); // mpz_swap is O(1), while mpz_set is O(n) where n is the number of limbs
}

int interact(mpz_class &c, mpz_class &m)
{
    // cout << "In interact" << endl;
    // interact with 61061.D
	gmp_fprintf(target_in, "%0256ZX\n", c.get_mpz_t());
    //cout << hex << "C = " << c << endl;
    // cout << "Before flush" << endl;
	fflush(target_in);
    // cout << "After flush" << endl;
    
    // Print execution time
	int time;
	gmp_fscanf(target_out, "%d\n%ZX", &time, m.get_mpz_t());
	cout << dec << "Execution time: " << time << "\n";
    return time;
}

int calibrate(mpz_class &c, mpz_class &N, mpz_class &d, mpz_class &m)
{
    //cout << "In calibrate" << endl;
    // interact with 61061.R
	gmp_fprintf(target_in, "%0256ZX\n%0256ZX\n%0256ZX\n", c.get_mpz_t(), N.get_mpz_t(), d.get_mpz_t());
    //cout << "Before flush" << endl;
	fflush(target_in);
    //cout << "After flush" << endl;
    
    // Print execution time
	int time;
	gmp_fscanf(target_out, "%d\n%ZX", &time, m.get_mpz_t());
	cout << dec << "Execution time: " << time << "\n";
    return time;
}

void attack(char* argv2)
{
    // count the number of interactions with the target
    unsigned int interaction_number = 0;
    
	// interact with 61061.conf
    // reading the input
	ifstream config (argv2, ifstream::in);
	mpz_class N, e;
	config >> hex >> N >> e;
    
    vector<mpz_class> cs, part_cs_mul_sq, part_cs_sq;
    vector<int> times;
    
    mpz_class c, m;
    int time_c;
    gmp_randclass randomness (gmp_randinit_default);
    
    mpz_class rho_sq;
    mp_limb_t omega;
    montgomery_omega(omega, N);
    montgomery_rho_sq(rho_sq, N);
    vector<bool> d;
    
    for (int j = 0; j < 1000; j++)
    {
        c = randomness.get_z_range(N);
        time_c = interact(c, m);
        c = montgomery_number(c, rho_sq, omega, N);
        cs.push_back(c);
        times.push_back(time_c);
    }
    //cout << hex << "Message: " << m << endl;
   
    ////////////////////////////////////////////////////////
    // ATTACK                                             //
    ////////////////////////////////////////////////////////
    
    // k_0 = 1
    for (int j = 0; j < 1000; j++)
    {
        mpz_class x;
        
        // SQUARE
        x = montgomery_multiplication(cs[0],cs[0],omega,N);
        
        // MULTIPLY
        x = montgomery_multiplication(x,cs[0],omega,N);
        
        // MODULAR REDUCTION
        if (x > N)
            x = x % N;
        
        part_cs_mul_sq.push_back(x);
    }
    d.push_back(1);
    
    mpz_class prev_c;
    
    for (int i = 1; i < 1024; i++)
    {
        // bit is 1
        int time1 = 0, time1red = 0;
        int time1_count = 0, time1red_count = 0;
        
        // bit is 0
        int time0 = 0, time0red = 0;
        int time0_count = 0, time0red_count = 0;
        
        //vector<bucket> bucket1, bucket2, bucket3, bucket4;

        for (int j = 0; j < 1000; j++)
        {    
            mpz_class x;
            int current_time = times[j];
            
            // case based on previous bit
            if (d[i-1] == 0)
                prev_c = part_cs_sq[j];
            else
                prev_c = part_cs_mul_sq[j];
            
            //////////////////////////////////////////////////////
            // CASE WHERE
            // k_i = 0
            // SQUARE
            x = montgomery_multiplication(prev_c,prev_c,omega,N);
            
            // MODULAR REDUCTION
            if (x >= N)
            {
                x = x % N;
                time0red += current_time;
                time0red_count++;
            }
            else
            {
                time0 += current_time;
                time0_count++;
            }
            
            part_cs_sq.push_back(x);
            
            
            //////////////////////////////////////////////////////
            // CASE WHERE
            // k_i = 1
            // SQUARE
            x = montgomery_multiplication(prev_c,prev_c,omega,N);
            
            bool hasRed = false;
            if (x >= N)
            {
                x = x % N;
                hasRed = true;
                time1red += current_time;
                time1red_count++;
            }
            
            // MULTIPLY
            x = montgomery_multiplication(x,cs[j],omega,N);
            
            // MODULAR REDUCTION
            if (x >= N)
            {
                x = x % N;
                if (!hasRed)
                {
                    time1red_count++;
                    time1red += current_time;
                }
            }
            else if (!hasRed)
            {
                time1_count++;
                time1 += current_time;
            }
            
            part_cs_mul_sq.push_back(x);    
        }
        
        if (time1_count != 0)
            time1 = time1/time1_count;
        
        if (time0_count != 0)
            time0 = time0/time0_count;
        
        if (time1red_count != 0)
            time1red = time1red/time1red_count;
        
        if (time0red_count != 0)
            time0red = time0red/time0red_count;
        
        if (abs(time1-time1red) > abs(time0-time0red))
            d.push_back(1);
        else
            d.push_back(0);
    }
    
    cout << "d = ";
    for (int j = 0; j < d.size(); j++)
        cout << d[j];
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