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

int interact(mpz_class &c, mpz_class &m, unsigned int &interaction_number);
void attack(char* argv2);
void attackR(char* argv2);
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
     
    return r;
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
    return montgomery_multiplication(num, rho_sq, omega, N) % N;
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

// Convert vector of bools to a number
mpz_class vec_to_num(const vector<bool> &d)
{
    mpz_class d_num = 0;
    for (bool bit : d)
        d_num = d_num * 2 + bit;
    return d_num;
}

bool verify(const mpz_class &e, const mpz_class &N, const mpz_class &sk, unsigned int &interaction_number)
{
    // decrypt ciphertext manually
    mpz_class c = 0b1010;
    mpz_class c_prime, m, m_prime;
    mpz_powm(m.get_mpz_t(), c.get_mpz_t(), sk.get_mpz_t(), N.get_mpz_t());
    
    // decrypt the same ciphertext with the oracle
    interact(c, m_prime, interaction_number);
    
    // check if the two messages are the same
    if (m == m_prime)
        return true;
    else
        return false;
}

int interact(mpz_class &c, mpz_class &m, unsigned int &interaction_number)
{
    // interact with 61061.D
	gmp_fprintf(target_in, "%0256ZX\n", c.get_mpz_t());
	fflush(target_in);
    
    // Print execution time
	int time;
	gmp_fscanf(target_out, "%d\n%ZX", &time, m.get_mpz_t());
	//cout << dec << "Execution time: " << time << "\n";
    interaction_number++;
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

void attackR(char* argv2)
{
    // interact with 61061.conf
    // reading the input
	ifstream config (argv2, ifstream::in);
	mpz_class N, e;
	config >> hex >> N >> e;
    
    // declare variables for communication with the target
    mpz_class c = 0b00, m, d;
    int time, time1, time2, time3, time4, time5, time6, time7;
    vector<int> times;
    
    d = 0b1001;
    time1 = calibrate(c, N, d, m);
    cout << "Time for d = 1001: " << time1 << endl;
    times.push_back(time1); 
    d = 0b1010;
    time = calibrate(c, N, d, m);
    cout << "Time for d = 1010: " << time << endl;
    times.push_back(time);
    d = 0b1100;
    time = calibrate(c, N, d, m);
    cout << "Time for d = 1100: " << time << endl;
    times.push_back(time);
    d = 0b1000;
    time2 = calibrate(c, N, d, m);
    cout << "Time for d = 1000: " << time2 << endl;
    cout << "Difference between 1001 and 1000: " << time1 - time2 << endl;
    d = 0b100;
    time4 = calibrate(c, N, d, m);
    cout << "Time for d = 100: " << time4 << endl;
    cout << "Difference between 100 and 1001: " << time1 - time4 << endl;
    cout << "Difference between 100 and 1100: " << time - time4 << endl;
    cout << "Difference between 100 and 1000: " << time2 - time4 << endl;
    d = 0b1110;
    time3 = calibrate(c, N, d, m);
    cout << "Time for d = 1110: " << time3 << endl;
    cout << "Difference between 1110 and  1001: " << time3 - time1 << endl;
    d = 0b1;
    time5 = calibrate(c, N, d, m);
    cout << "Time for d = 1: " << time5 << endl;
    d = 0b10;
    time6 = calibrate(c, N, d, m);
    cout << "Time for d = 10: " << time6 << endl;
    d = 0b11;
    time7 = calibrate(c, N, d, m);
    cout << "Time for d = 11: " << time7 << endl;
    
    time = 0;
    for (int i = 0; i < 3; i++)
    {
        time += times[i];
    }
    cout << "Average time: " << time/3 << endl;
    
    
    
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
    
    // execution times for the initial sample set of ciphertexts
    vector<int> times;
    
    // declare variables for communication with the target
    mpz_class c=0, m;
    int time_c;
    
    // time got from 61061.R
    int time_op = 3770, time_overhead = 2*time_op;
    // get execution time
    int time_ex = interact(c, m, interaction_number);
    // No of (bits + bits set)
    int bits_num = (time_ex - time_overhead)/time_op;
    cout << "Upper bound on bits: " << bits_num << endl;
    
    // vectors of ciphertexts
    vector<mpz_class> cs;
    vector<vector<mpz_class>> part_cs_mul_sq(bits_num), part_cs_sq(bits_num);
    
    // produce random ciphertexts
    gmp_randclass randomness (gmp_randinit_default);
    
    // Montgomery preprocessing
    mpz_class rho_sq;
    mp_limb_t omega;
    montgomery_omega(omega, N);
    montgomery_rho_sq(rho_sq, N);
    
    // d is the private key
    vector<bool> d;
    int oracle_queries = 1000;
    
    // initial sample set and respective execution times
    for (int j = 0; j < oracle_queries; j++)
    {
        c = randomness.get_z_range(N);
        time_c = interact(c, m, interaction_number);
        c = montgomery_number(c, rho_sq, omega, N);
        cs.push_back(c);
        times.push_back(time_c);
        c = montgomery_multiplication(c, c, omega, N);
        
        // will be used when prev d_i = 1
        part_cs_mul_sq[0].push_back(c);
        
        part_cs_sq[0].push_back(0);
    }
   
    ////////////////////////////////////////////////////////
    // ATTACK                                             //
    ////////////////////////////////////////////////////////
    
    d.push_back(1);
    bits_num -= 2;
    
    mpz_class prev_c;
    bool isKey = false, doResample = false;
    
    // bit and backtrack counter
    int bit_i = 0, backtracks = 0;
    vector<bool> isFlipped(bits_num, false);
    vector<int> timeDiff;
    
    while (!isKey)
    {
        bit_i++;
        
        if (doResample)
        {
            cout << "RESAMPLING\n"; 
            for (int j = 0; j < 250; j++)
            {
                c = randomness.get_z_range(N);
                time_c = interact(c, m, interaction_number);
                c = montgomery_number(c, rho_sq, omega, N);
                cs.push_back(c);
                times.push_back(time_c);
                c = montgomery_multiplication(c, c, omega, N);
                
                // will be used when prev d_i = 1
                part_cs_mul_sq[0].push_back(c);
                
                part_cs_sq[0].push_back(0);
            }

            oracle_queries += 250;
            fill(isFlipped.begin(), isFlipped.end(), false);
            bit_i = 0;
            bits_num = (time_ex - time_overhead)/time_op - 2;
            d.clear();
            d.push_back(1);
            doResample = false;
            backtracks = 0;
            continue;
        }
        
        // bit is 1
        int time1 = 0, time1red = 0;
        int time1_count = 0, time1red_count = 0;
        
        // bit is 0
        int time0 = 0, time0red = 0;
        int time0_count = 0, time0red_count = 0;

        //#pragma omp parallel for
        for (int j = 0; j < oracle_queries; j++)
        {
            mpz_class x;
            int current_time = times[j];
            
            // case based on previous bit
            if (d.back() == 0)
                prev_c = part_cs_sq[bit_i-1][j];
            else
                prev_c = part_cs_mul_sq[bit_i-1][j];
            
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
            
            if(part_cs_sq[bit_i].size() <= j)
                part_cs_sq[bit_i].push_back(x);
            else
                part_cs_sq[bit_i][j] = x;
            
            
            
            //////////////////////////////////////////////////////
            // CASE WHERE
            // k_i = 1
            
            // MULTIPLY
            x = montgomery_multiplication(prev_c,cs[j],omega,N);
            
            //MODULAR REDUCTION
            if (x >= N)
                x = x % N;
            
            // SQUARE
            x = montgomery_multiplication(x,x,omega,N);
            
            //MODULAR REDUCTION
            if (x >= N)
            {
                x = x % N;
                time1red += current_time;
                time1red_count++;
            }
            else
            {
                time1 += current_time;
                time1_count++;
            }
            
            if(part_cs_mul_sq[bit_i].size() <= j)
                part_cs_mul_sq[bit_i].push_back(x);
            else
                part_cs_mul_sq[bit_i][j] = x;   
        }
        
        if (time1_count != 0)
            time1 = time1/time1_count;
        
        if (time0_count != 0)
            time0 = time0/time0_count;
        
        if (time1red_count != 0)
            time1red = time1red/time1red_count;
        
        if (time0red_count != 0)
            time0red = time0red/time0red_count;
        
        cout << " time1 = " << time1;
        cout << " time1red = " << time1red;
        cout << " time0 = " << time0;
        cout << " time0red = " << time0red;
        cout << " Diff = " << abs(time1-time1red) - abs(time0-time0red);
        cout << " backtracks = " << backtracks;
        
        timeDiff.push_back(abs(abs(time1-time1red) - abs(time0-time0red)));
        
        // find local mean instead of a set threshold
        int localTimeMean;
        
        if (backtracks > 0)
        {
            localTimeMean = 0;
            for (int k = 0; k < backtracks; k++)
                localTimeMean += timeDiff[bit_i - k];
            
            localTimeMean /= backtracks;
            cout << "local time mean = " << localTimeMean << "\n";
        }
        else
            localTimeMean = timeDiff.back();
        
        if(abs(abs(time1-time1red) - abs(time0-time0red)) > 3 && bits_num > 0)
        {
            if (abs(time1-time1red) > abs(time0-time0red))
            {
                d.push_back(1);
                cout << " d = 1\n";
                bits_num-=2;  
            }
            else
            {
                d.push_back(0);
                cout << " d = 0\n";
                bits_num--;
            }
            
            if(isFlipped[bit_i])
            {
                isFlipped[bit_i] = false;
                backtracks = 0;
            }
        }
        else
        {
            bit_i--;
            
            while(isFlipped[bit_i])
            {
                bits_num += d.back() + 1;
                d.pop_back();
                bit_i--;
                backtracks++;
            }
            
            if (backtracks > 2)
            {
                doResample = true;
                continue;
            }
            
            if (d.back())
            {
                d.pop_back();
                d.push_back(0);
                bits_num++;
            }
            else
            {
                d.pop_back();
                d.push_back(1);
                bits_num--;
            }
            isFlipped[bit_i] = true; 
        }
        
        // check if we have recovered the full private key
        if(bits_num == 0)
        {
            mpz_class sk = vec_to_num(d);
            isKey = verify(e, N, sk, interaction_number);
        }
    }
    
    cout << "\nd = ";
    for (int j = 0; j < d.size(); j++)
        cout << d[j];
    
    cout << "\n Interactions: " << interaction_number;
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
