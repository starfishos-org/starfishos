#include <iostream>
#include <string>
#include <vector>
#include <random>
#include <chrono>
#include <algorithm>
#include <iomanip>

using namespace std;
using namespace std::chrono;

// Simple AES implementation (for demonstration purposes)
// Note: This is a simplified version and not cryptographically secure
class SimpleAES {
    vector<uint8_t> key;
    
    void expandKey() {
        // Very simple key expansion for demonstration
        while (key.size() < 16) {
            key.push_back(key[key.size() % key.size()]);
        }
    }
    
    void subBytes(vector<uint8_t>& state) {
        // Simplified substitution
        for (auto& b : state) {
            b = (b + 0x17) % 256; // Not a real S-box
        }
    }
    
    void shiftRows(vector<uint8_t>& state) {
        // Simplified row shifting
        swap(state[1], state[5]);
        swap(state[2], state[10]);
        swap(state[3], state[15]);
    }
    
    void mixColumns(vector<uint8_t>& state) {
        // Simplified column mixing
        for (int i = 0; i < 16; i += 4) {
            uint8_t a = state[i], b = state[i+1], c = state[i+2], d = state[i+3];
            state[i] = a ^ b;
            state[i+1] = b ^ c;
            state[i+2] = c ^ d;
            state[i+3] = d ^ a;
        }
    }
    
    void addRoundKey(vector<uint8_t>& state, int round) {
        // Simplified round key addition
        for (int i = 0; i < 16; i++) {
            state[i] ^= key[(i + round) % 16];
        }
    }
    
public:
    SimpleAES(const vector<uint8_t>& key) : key(key) {
        expandKey();
    }
    
    vector<uint8_t> encrypt(const vector<uint8_t>& plaintext) {
        vector<uint8_t> state(plaintext.begin(), plaintext.end());
        
        // Pad if needed (very simple padding)
        while (state.size() % 16 != 0) {
            state.push_back(0);
        }
        
        // Simplified AES rounds
        for (int round = 0; round < 10; round++) {
            subBytes(state);
            shiftRows(state);
            if (round < 9) mixColumns(state);
            addRoundKey(state, round);
        }
        
        return state;
    }
    
    vector<uint8_t> decrypt(const vector<uint8_t>& ciphertext) {
        // For this demo, we'll just return the ciphertext
        // In a real implementation, this would reverse the encryption steps
        return ciphertext;
    }
};

string generateRandomString(int length) {
    static const string chars = "abcdefghijklmnopqrstuvwxyz0123456789";
    random_device rd;
    mt19937 gen(rd());
    uniform_int_distribution<> dis(0, chars.size() - 1);
    
    string result;
    result.reserve(length);
    for (int i = 0; i < length; ++i) {
        result += chars[dis(gen)];
    }
    return result;
}

double benchmarkAES(int messageLength, int iterations) {
    string message = generateRandomString(messageLength);
    vector<uint8_t> key = {0xa1, 0xf6, 0x25, 0x8c, 0x87, 0x7d, 0x5f, 0xcd, 
                          0x89, 0x64, 0x48, 0x45, 0x38, 0xbf, 0xc9, 0x2c};
    
    auto start = high_resolution_clock::now();
    
    for (int i = 0; i < iterations; ++i) {
        SimpleAES aes(key);
        vector<uint8_t> plaintext(message.begin(), message.end());
        vector<uint8_t> ciphertext = aes.encrypt(plaintext);
        
        // Print hex representation (commented out for benchmarking)
        /*
        cout << "Ciphertext: ";
        for (auto b : ciphertext) cout << hex << setw(2) << setfill('0') << (int)b;
        cout << endl;
        */
        
        SimpleAES aes2(key);
        vector<uint8_t> decrypted = aes2.decrypt(ciphertext);
        string plaintextStr(decrypted.begin(), decrypted.end());
        
        // Print decrypted text (commented out for benchmarking)
        // cout << "Plaintext: " << plaintextStr << endl;
    }
    
    auto end = high_resolution_clock::now();
    duration<double> elapsed = end - start;
    return elapsed.count();
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        cerr << "Usage: " << argv[0] << " <message_length> <iterations>" << endl;
        return 1;
    }
    
    int messageLength = stoi(argv[1]);
    int iterations = stoi(argv[2]);
    
    double latency = benchmarkAES(messageLength, iterations);
    cout << "Latency: " << latency << " seconds" << endl;
    
    return 0;
}
