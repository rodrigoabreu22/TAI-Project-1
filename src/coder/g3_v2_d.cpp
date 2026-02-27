#define _CRT_SECURE_NO_WARNINGS

#include <stdlib.h>
#include <cstdio>
#include <locale.h>
#include <math.h>
#include <string>
#include <iostream>
#include <fstream>
#include <vector>
#include <string.h>
#include <iomanip>
#include <stdexcept>

using namespace std;

struct Statistics {
    long long original_size;
    long long compressed_size;
    double compression_ratio;
    long long space_saved;
};

static long long file_size(const string& path) {
    ifstream f(path, ios::binary | ios::ate);
    if (!f) {
        throw runtime_error("Cannot open file: " + path);
    }
    return static_cast<long long>(f.tellg());
}
//
class Fileread {
private:
    struct byte {
        unsigned char b;
        int len;
    };
    FILE *f;
    string filename, filetype;
    byte br;
    int filesize;
    int calc_filesize() {
        int n = 0, ans = 0;
        const int TMPBUFSZ = 512;
        char *tmpbuf = new char[TMPBUFSZ];
        while (n = fread(tmpbuf, 1, TMPBUFSZ, f)) {
            ans += n;
        }
        fseek(f, 0, SEEK_SET);
        delete[] tmpbuf;
        return ans;
    }
public:
    Fileread(string s, string type) : filesize(-1), br{ 0, 0 } {
        filename = s;
        filetype = type;
        f = fopen(s.c_str(), type.c_str());
        if (f == NULL) {
            cerr << "error in opening file" << endl;
            exit(1);
        }
    }
    Fileread(char *s, char *type) : filesize(-1), br{ 0, 0 } {
        filename = s;
        filetype = type;
        f = fopen(s, type);
        if (f == NULL) {
            cerr << "error in opening file" << endl;
            exit(1);
        }
    }
    Fileread() {}
    int get_filesize() {
        if (filesize == -1)
            filesize = calc_filesize();
        return filesize;
    }
    int read(unsigned char *buf, int len) {
        return fread(buf, 1, len, f);
    }
    int read(int *x) {
        return fread(x, sizeof(*x), 1, f);
    }
    int read(long long *x) {
        return fread(x, sizeof(*x), 1, f);
    }
    int bread(int *a) {
        int ans = 0;
        if (br.len == 0) {
            if (fread(&br.b, 1, 1, f) != 1) {
                return 0;
            }
            br.len = 8;
        }
        br.len--;
        ans <<= 1;
        ans |= br.b & 1;
        br.b >>= 1;
        *a = ans;
        return 1;
    }
    void fflush() {
        br.len = 0;
    }
    int bread(int *a, int n) {
        *a = 0;
        for (int i = 0; i < n; i++) {
            int bit_r;
            if (!bread(&bit_r))
                return 0;
            *a |= bit_r * (1 << i);
        }
        return 1;
    }
    void seek(int offset, int mode) {
        fseek(f, offset, mode);
    }
    void operator=(const Fileread &copy) {
        f = fopen(copy.filename.c_str(), copy.filetype.c_str());
        filesize = copy.filesize;
        filename = copy.filename;
        filetype = copy.filetype;
        br = copy.br;
    }
};

class Filewrite {
private:
    struct byte {
        unsigned char b;
        int len;
    };
    FILE *f;
    byte bw;
    string filename, filetype;
    int filesize;
public:
    Filewrite(string s, string type) : bw{ 0, 0 }, filesize(0) {
        filename = s;
        filetype = type;
        f = fopen(s.c_str(), type.c_str());
        if (f == NULL) {
            cerr << "error in opening file" << endl;
            exit(1);
        }
    }
    Filewrite(char *s, char *type) : bw{ 0, 0 }, filesize(0) {
        filename = s;
        filetype = type;
        f = fopen(s, type);
        if (f == NULL) {
            cerr << "error in opening file" << endl;
            exit(1);
        }
    }
    ~Filewrite() {
        fclose(f);
    }
    void writebite(int bit) {
        filesize++;
        if (bit == -1) {
            fwrite(&bw.b, 1, 1, f);
            bw.len = 0;
            bw.b = 0;
        }
        else {
            bw.b |= bit << (bw.len++);
            if (bw.len == 8) {
                fwrite(&bw.b, 1, 1, f);
                bw.len = 0;
                bw.b = 0;
            }
        }
    }
    void write(unsigned char x) {
        fwrite(&x, sizeof(x), 1, f);
    }
    void write(int x) {
        fwrite(&x, sizeof(x), 1, f);
    }
    Filewrite() {}
    void operator=(const Filewrite &copy) {
        f = fopen(copy.filename.c_str(), copy.filetype.c_str());
        filesize = copy.filesize;
        filename = copy.filename;
        filetype = copy.filetype;
        bw = copy.bw;
    }
};

class Probability {
private:
    const int CHARSIZ = 256;
    int add, del, overflo;
    int *p, *psum;
public:
    Probability(int add = 1000, int del = 3000, int overflo = 3000) : add(add), del(del), overflo(overflo) {
        p = new int[CHARSIZ + 1];
        psum = new int[CHARSIZ + 1];
        p[0] = 1;
        psum[0] = 0;
        for (int i = 1; i <= CHARSIZ; i++) {
            p[i] = 1;
            psum[i] = psum[i - 1] + p[i];
        }
    }
    Probability(const Probability &copy) {
        p = new int[CHARSIZ + 1];
        psum = new int[CHARSIZ + 1];
        add = copy.add;
        del = copy.del;
        overflo = copy.overflo;
        for (int i = 0; i <= CHARSIZ; i++) {
            p[i] = copy.p[i];
            psum[i] = copy.psum[i];
        }
    }
    ~Probability() {
        delete[] p;
        delete[] psum;
    }
    void operator =(const Probability &copy) {
        add = copy.add;
        del = copy.del;
        overflo = copy.overflo;
        for (int i = 0; i <= CHARSIZ; i++) {
            p[i] = copy.p[i];
            psum[i] = copy.psum[i];
        }
    }
    void check_overflow() {
        if (psum[CHARSIZ] > overflo) {
            for (int i = 1; i <= CHARSIZ; i++) { //Нулевой символ занулить
                p[i] /= del;
                if (p[i] == 0) {
                    p[i] = 1;
                }
                psum[i] = psum[i - 1] + p[i];
            }
        }
    }
    void inc(int i) {
        p[i] += add;
        for (int q = i; q <= CHARSIZ; q++) {
            psum[q] = psum[q - 1] + p[q];
        }
        check_overflow();
    }
    void get_borders(long long &lnew, long long &rnew, int i) {
        long long l = lnew, r = rnew;
        long long len = (r - l + 1);
        lnew = l + len * psum[i - 1] / psum[CHARSIZ];
        rnew = l + len * psum[i] / psum[CHARSIZ] - 1;
    }
    void set_add(int add_) {
        add = add_;
    }
};

class Compressor {
private:
    const long long LEN, MAX;
    long long l, r, qtr1, qtr2, qtr3, half, bufsize;
    int bits_to_folow = 0;
    unsigned char *buf;
    vector<int> agres;
    Probability prob;
    Fileread fr;
    Filewrite fw;
    void addbits(int &bits_to_folow, int last) {
        for (int i = 0; i < bits_to_folow; i++) {
            fw.writebite(!last);
        }
        bits_to_folow = 0;
    }
    long long check(int n, const unsigned char buf[], long long l, long long r, long long add, Probability prob) {
        long long half = (MAX + 1) >> 1, qtr1 = half >> 1, qtr3 = qtr1 * 3;
        long long size = 0;
        prob.set_add(add);
        for (int i = 0; i < n; i++) {
            int j = buf[i] + 1;
            prob.get_borders(l, r, j);
            while (1) {
                if (r < half) {
                    size++;
                }
                else if (l >= half) {
                    size++;
                    l -= half;
                    r -= half;
                }
                else if (l >= qtr1 && r < qtr3) {
                    l -= qtr1;
                    r -= qtr1;
                    size++;
                }
                else break;
                l += l;
                r += r + 1;
            }
            prob.inc(j);
        }
        return size;
    }
    long long findbest(const unsigned char buf[], long long l, long long r, long long n, const Probability &prob) {
        Probability checkprob;
        long long size = 0, add = -1, minsize, fl = 1, last, sign, min_st;
        for (long long i = 1, st = 0; i <= 4294967296; i *= 2, st++) {
            checkprob = prob;
            long long size = check(n, buf, l, r, i, checkprob);
            if (i == 4) {
                if (last > size) {
                    sign = 1;
                }
                else {
                    sign = -1;
                }
            }
            if (i > 4) {
                if (sign * (last - size) < 0) {
                    break;
                }
            }
            if (add == -1 || size < minsize) {
                add = i;
                min_st = st;
                minsize = size;
            }
            last = size;
        }
        return min_st;
    }
    void get_agr() {
        Fileread f = fr;
        int n;
        while (n = f.read(buf, bufsize)) {
            Probability prob;
        }
    }
public:
    Compressor(string ifile, string ofile, long long len, long long bufsize = 512) :bufsize(bufsize), LEN(len), MAX(((long long)1 << len) - 1) {
        buf = new unsigned char[bufsize];
        fr = Fileread(ifile, "rb");
        fw = Filewrite(ofile, "wb");
        fw.write(fr.get_filesize());
        l = 0;
        r = MAX;
        half = (r + 1) >> 1;
        qtr1 = half >> 1;
        qtr3 = qtr1 * 3;
    }
    Compressor() : LEN(0), MAX(0) {}
    ~Compressor() {
        delete[] buf;
    }
    void compress() {
        int n, itt_count = 0;
        while (n = fr.read(buf, bufsize)) {
            int min_st = findbest(buf, 0, MAX, n, prob);
            agres.push_back(min_st);
            prob.set_add(1 << min_st);
            for (int i = 0; i < n; i++) {
                int j = buf[i] + 1;
                long long f_flag;
                prob.get_borders(l, r, j);
                while (1) {
                    if (r < half) {
                        fw.writebite(0);
                        addbits(bits_to_folow, 0);
                    }
                    else if (l >= half) {
                        fw.writebite(1);
                        addbits(bits_to_folow, 1);
                        l -= half;
                        r -= half;
                    }
                    else if (l >= qtr1 && r < qtr3) {
                        l -= qtr1;
                        r -= qtr1;
                        bits_to_folow++;
                    }
                    else break;
                    l += l;
                    r += r + 1;
                }
                prob.inc(j);
            }
        }
        fw.writebite(1);
        addbits(bits_to_folow, 1);
        fw.writebite(-1);
        for (int i = 0; i < agres.size(); i++) {
            for (int j = 0; j < 5; j++) {
                fw.writebite(agres[i] & 1);
                agres[i] >>= 1;
            }
        }
        fw.writebite(-1);
    }
};

class Decompressor {
private:
    const int CHARSIZ = 256;
    const long long LEN, MAX;
    long long l, r, qtr1, qtr2, qtr3, half, bufsize;
    int initsize;
    unsigned char *buf;
    vector<int> agres;
    Probability prob;
    Fileread fr;
    Filewrite fw;
    void readagr() {
        long long n_agr = (initsize + bufsize - 1) / bufsize;
        fr.seek(0, SEEK_END);
        fr.seek(-((n_agr * 5 + 7) / 8) * 1, SEEK_CUR);
        for (int i = 0; i < n_agr; i++) {
            int tmpagr;
            fr.bread(&tmpagr, 5);
            agres.push_back(tmpagr);
        }
        fr.fflush();
    }
public:
    Decompressor(string ifile, string ofile, long long len, long long bufsize = 512) : l(l), r(r), bufsize(bufsize), LEN(len), MAX(((long long)1 << len) - 1) {
        buf = new unsigned char[bufsize];
        fr = Fileread(ifile, "rb");
        fw = Filewrite(ofile, "wb");
        fr.read(&initsize);
        l = 0;
        r = MAX;
        half = (r + 1) >> 1;
        qtr1 = half >> 1;
        qtr3 = qtr1 * 3;
    }
    Decompressor() : LEN(0), MAX(0) {}
    ~Decompressor() {
        delete[] buf;
    }
    void decompress() {
        readagr();
        fr.seek(sizeof(initsize), SEEK_SET);
        long long val = 0, m_agr = 0;
        for (int j = LEN - 1; j >= 0; j--) {
            int bit_r;
            if (fr.bread(&bit_r)) {
                val |= ((long long)1 << j) * bit_r;
            }
        }
        for (int i = 0; i < initsize; i++) {
            if (i % bufsize == 0) {
                int st;
                st = agres[m_agr++];
                prob.set_add(1 << st);
            }
            int bit = 0, find = 0, flag = 1, rd = 0, mask = 0;
            int u = 1;
            for (int j = 1; j <= CHARSIZ; j++) {
                long long ll = l, rr = r;
                prob.get_borders(ll, rr, j);
                if (ll <= val && val <= rr) {
                    unsigned char Cout = j - 1;
                    prob.inc(j);
                    fw.write(Cout);
                    l = ll;
                    r = rr;
                    break;
                }
            }
            while (1) {
                if (r < half) {
                }
                else if (l >= half) {
                    l -= half;
                    r -= half;
                    val -= half;
                }
                else if (l >= qtr1 && r < qtr3) {
                    l -= qtr1;
                    r -= qtr1;
                    val -= qtr1;
                }
                else break;
                l += l;
                r += r + 1;
                val += val;
                int bit_r;
                if (fr.bread(&bit_r)) {
                    val |= bit_r;
                }
            }

        }
    }

};

void compress_ari(char *ifile, char *ofile) {
    Compressor c(ifile, ofile, 31, 512);
    c.compress();
}

void decompress_ari(string ifile, string ofile) {
    Decompressor c(ifile, ofile, 31, 512);
    c.decompress();
}

int main(int argc, char **argv) {
    if (argc != 3) {
        cerr << "Usage: " << argv[0] << " <input_file> <output_file>" << endl;
        return 1;
    }

    try {
        decompress_ari(argv[1], argv[2]);
        cout << "Decompressed to: " << argv[2] << endl;
    } catch (const exception& e) {
        cerr << "Error: " << e.what() << endl;
        return 1;
    }

    return 0;
}
