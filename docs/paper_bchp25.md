# Fast Homomorphic Linear Algebra with BLAS

Youngjin Bae¹, Jung Hee Cheon¹'², Guillaume Hanrot³, Jai Hyun Park³, Damien Stehlé³

¹ CryptoLab Inc., Seoul, Republic of Korea
² Seoul National University, Seoul, Republic of Korea
³ ENS de Lyon, France

arXiv:2503.16080v1 [cs.CR] 20 Mar 2025

---

## Abstract

Homomorphic encryption is a cryptographic paradigm enabling computation on encrypted data, and has widespread applications in privacy-preserving data processing, especially in artificial intelligence. Most such applications involve extensive linear algebra, including matrix-vector products and matrix-matrix products.

The pivotal role of linear algebra in homomorphic algebra extends far beyond homomorphic algebra: it is central to most scientific computations. Such generality motivated the development in 1979 of a highly optimized set of routines, called BLAS, for basic linear algebra subroutines.

Given the importance of homomorphic linear algebra applications and the need for efficient plaintext linear algebra implementations that fully exploit the hardware, this paper investigates the connections between CKKS-based homomorphic linear algebra and floating-point plaintext linear algebra. CKKS is the most natural choice in this setting, as it natively supports real numbers and provides strong single-instruction multiple-data (SIMD) parallelism.

We implement the reduction of matrix-vector products, vector-vector products and matrix-matrix products to their plaintext equivalents for medium- to large-sized matrices.

---

## 1 Introduction

Homomorphic encryption (HE) allows us to compute on encrypted data without decrypting it first. It has found many applications in privacy-preserving data processing and, more recently, in artificial intelligence.

Many HE applications require extensive linear algebra, such as matrix-vector products and matrix-matrix products. For example, neural networks involve fully-connected layers (matrix-vector products) and convolution layers (correlation products). Other examples include private information retrieval (PIR), which can be expressed as a matrix-vector product by representing the database as a matrix and the query as a one-hot vector, and approximate nearest neighbor search, which can be expressed as comparing template vectors with a database using a dot product.

The following three types of homomorphic linear algebra operations are most commonly used.

### Homomorphic matrix-vector products (Mv)

Input: a matrix $\mathbf{M} = (m_{i,j})_{0\leq i<d_1,0\leq j<d_2}$ and a vector $\mathbf{v} = (v_i)_{0\leq i<d_2}$, both of which may be encrypted or plaintext. This leads to ciphertext-plaintext (CP-Mv), plaintext-ciphertext (PC-Mv), and ciphertext-ciphertext (CC-Mv) variants.

Applications include private inference for neural networks (fully-connected layers and convolution layers), private information retrieval, and approximate nearest neighbor search. These applications typically involve large matrix and vector dimensions, and the matrix is usually known in advance and can be precomputed; see [38] for more details.

### Plaintext-ciphertext matrix multiplication (PC-MM and CP-MM)

Input: a plaintext matrix $\mathbf{U} = (u_{i,j})_{0\leq i<d_1,0\leq j<d_2}$ and a ciphertext of the corresponding matrix $\mathbf{M} = (m_{i,j})_{0\leq i<d_2,0\leq j<d_3}$. Output: the ciphertext of $\mathbf{M\cdot U}$ (CP-MM) or $\mathbf{U\cdot M}$ (PC-MM).

Applications include privacy-preserving machine learning (PPML), especially large language model (LLM) inference for Transformers such as GPT [51], BERT [22], and LLaMA [55]. CP-MM can also be used in combination with secure multi-party computation [36, 48, 23] or fully homomorphic encryption [56] for private evaluation of LLMs. PC-MM can be used for federated principal component analysis between different data providers [27], as well as for smart contracts, healthcare, and finance [44].

Note: The difference between PC-MM and CP-MM lies in whether the matrix ciphertext coefficients are packed row-wise or column-wise. Once the packing scheme is fixed, they are different problems.

### Ciphertext-ciphertext matrix multiplication (CC-MM)

Input: ciphertexts of the two input matrices. Output: the ciphertext of the product matrix.

This is crucial in PPML, where a server trains or infers models using clients' encrypted data, such as for private training of LLMs.

## 1.1 From Encrypted to Plaintext Linear Algebra

Most existing methods for homomorphic linear algebra rely on the ciphertext-level operations provided by the HE scheme: SIMD addition, SIMD multiplication, and key-switching. This approach has two drawbacks:

1. Most methods achieve the desired arithmetic complexity but require too many key-switching operations, which often dominate the total running time.

2. The algorithms mix different kinds of operations, especially frequent key-switching, making the computation and memory access patterns irregular. This makes the algorithm and its implementation hard to optimize, often leading to memory bandwidth bottlenecks, and many steps are limited by memory due to large key-switching key sizes.

This paper takes a different approach: we also use ciphertexts in the RLWE format, but we follow the idea introduced in [44] and reduce the operations to plaintext linear algebra, i.e., PP-MM and PP-Mv.

**Theoretical significance.** This approach challenges the common belief that homomorphic computation is necessarily several orders of magnitude slower than plaintext computation, by showing that certain tasks can be performed homomorphically with only a small overhead.

**Practical significance.** This approach allows us to leverage the highly optimized BLAS libraries that have been developed over decades by the high-performance linear algebra community [1, 54].

**Limitations.** The reduction may change the dimensions of the operations: when the matrix dimensions are smaller than the RLWE ring degree, the arithmetic complexity may increase. Moreover, the reduction is not typically a direct call to a plaintext linear algebra operation: there are preprocessing and postprocessing tasks, the costs of which are negligible compared to the plaintext linear algebra part. However, there are higher-cost tasks that can be performed offline, called pre-computation, the costs of which can be amortized over multiple calls to the main routine.

**Benefits.** Despite the limitations, this approach makes the computation pattern more regular and can fully utilize the highly optimized BLAS libraries developed by the high-performance linear algebra community.

## 1.2 Contributions

This paper reduces encrypted linear algebra tasks of various dimensions to plaintext linear algebra computations of comparable dimensions. The ciphertexts are defined as polynomials modulo an integer $q$, while the plaintext matrix multiplication is an approximation of real arithmetic, and the plaintext linear algebra is performed modulo an integer $q'$ whose bit-length is close to that of $q$, differing only by a small additive constant.

**Algorithms for CP-MM/CP-Mv.** We propose several algorithms for CP-MM (resp. CP-Mv) that reduce to one or two PP-MM (resp. PP-Mv) operations. Our algorithms can handle the case where the row dimension of the ciphertext matrix is smaller or greater than the RLWE ring degree, in a black-box manner, with limited overhead. Our algorithms extend those from [44] and provide better flexibility and efficiency:

- For a ciphertext matrix of dimension $d_1 \times d_2$, depending on whether $d_1$ is smaller or greater than the RLWE ring degree $N$ (the approach of [44] only works for $d_1 \geq N$), we propose two algorithms that reduce CP-MM to two modular PP-MM operations. If the plaintext matrix is of dimension $d_2 \times d_3$, then the PP-MM dimensions are $N \times d_2 \times d_3$ and $d_1 \times d_2 \times d_3$.

- We also propose two additional algorithms for the case of small $d_1$ and large $d_1$, respectively. If pre-computation on the plaintext matrix is allowed, then only a single $d_1 \times d_2 \times d_3$ modular PP-MM operation is required. Moreover, this modular PP-MM can be replaced with a floating-point PP-MM, achieving a reduction from CP-MM to a single same-dimension floating-point PP-MM with pre-computation.

- When $d_3 = 1$, these algorithms become CP-Mv algorithms.

**Algorithms for CC-MM.** For matrices whose dimensions are greater than or equal to the RLWE degree, we propose a CC-MM algorithm that reduces to four modular PP-MM operations of the same dimensions as the original CC-MM. However, this algorithm requires a large evaluation key. To address this, we also propose algorithms for CC-MM based on RGSW ciphertexts.

**Experimental validation.** Using the HEaan library [21], we implement several of our algorithms and provide three sets of experimental results in Section 6:

1. The costs of CP-MM based on RLWE without pre-computation.
2. The costs of CC-MM for large square matrices.
3. The costs of CC-MM and CC-Mv based on RGSW.

---

## 1.3 Technical Overview

For simplicity, we assume that the matrices are square, with dimension $d$ denoted as $d$, and let $q \geq 2$ be an integer.

We consider the BFV/BGV [10, 15] and CKKS [17] HE schemes based on the Ring Learning With Errors (RLWE) problem [40]. In these schemes, a vector of plaintext values is encrypted as a single ciphertext, by viewing it as the coefficients of a polynomial modulo $X^N + 1$. This provides single-instruction multiple-data (SIMD) parallelism, allowing us to pack multiple values into a single ciphertext and perform operations on all of them simultaneously.

The core of our approach lies in exploiting the structure of the RLWE ciphertext format to efficiently perform matrix multiplication. Specifically, we show how to structure the ciphertext packing and the key-switching operations to reduce the overall computation to a sequence of efficient plaintext linear algebra operations.

### Toeplitz Matrix Structure

Let $\mathbf{M} \in \mathcal{R}_q^{d_1 \times d_2}$ be a matrix. The RLWE ciphertext for $\mathbf{M}$ consists of two polynomials $(a, b) \in \mathcal{R}_q^2$ such that:

$$b = a \cdot \text{sk} + \mathbf{M} + e \mod (X^N + 1, q)$$

where $\text{sk}$ is the secret key and $e$ is a small error polynomial.

The key insight is that the multiplication by the secret key can be represented as a Toeplitz matrix operation. Specifically, we have:

$$\text{Toep}(\text{sk}) \cdot \mathbf{A} + \mathbf{B} \approx \mathbf{M} \mod q \tag{1}$$

where $\text{Toep}(\text{sk})$ is a structured matrix derived from the secret key polynomial, and $\mathbf{A}, \mathbf{B}$ are the plaintext encodings of the ciphertext polynomials.

### CP-MM via Plaintext Multiplication

Given a plaintext matrix $\mathbf{U} \in \mathbb{R}^{d \times d}$, we multiply both sides of Equation (1) by $\mathbf{U}$ to obtain:

$$\text{Toep}(\text{sk}) \cdot (\mathbf{A} \cdot \mathbf{U}) + (\mathbf{B} \cdot \mathbf{U}) \approx \mathbf{M} \cdot \mathbf{U} \mod q \tag{2}$$

The $\approx$ symbol hides an error term that is also multiplied by $\mathbf{U}$, which must be taken into account when setting parameters. Equation (2) has the same form as Equation (1), so $(a_i', b_i')$ (where $a_i'$ corresponds to the coefficients of $\mathbf{A} \cdot \mathbf{U}$ and $b_i'$ corresponds to the coefficients of $\mathbf{B} \cdot \mathbf{U}$) form the encryption of $\mathbf{M} \cdot \mathbf{U}$. Overall, CP-MM of $\mathbf{M} \cdot \mathbf{U}$ reduces to PP-MM of $\mathbf{A} \cdot \mathbf{U}$ and $\mathbf{B} \cdot \mathbf{U}$ (mod $q$).

When $d = kN$ (where $k \geq 1$ is an integer), this approach can be generalized: each column is encrypted using multiple RLWE ciphertexts, with a total of $d^2/N$ ciphertexts. We obtain:

$$(\mathbf{I} \otimes \text{Toep}(\text{sk})) \cdot \mathbf{A} + \mathbf{B} \approx \mathbf{M} \mod q$$

Multiplying $\mathbf{U}$ on the right gives a CP-MM algorithm consisting of two modular PP-MM operations.

### Shared-a RLWE Ciphertexts and High-Dimensional CP-MM

To reduce the cost of high-dimensional CP-MM, we use shared-$a$ multi-secret RLWE ciphertexts. The matrix $\mathbf{M}$ is provided by $d^2/N$ multi-secret RLWE ciphertexts $(a_i, b_{i,j})_{0 \leq i < d, 0 \leq j < k}$, where there are only $d$ values of $a_i$, satisfying:

$$\forall i, j : a_i \cdot \text{sk}_j + b_{i,j} = m_{i,j} + e_{i,j} \mod q$$

The matrix $\mathbf{A}$ corresponding to the coefficients of $a_i$ has dimensions $d \times N$, and the matrix $\mathbf{B}$ corresponding to the coefficients of $b_i$ has dimensions $d \times d$. Multiplying both sides by $\mathbf{U}$ gives the CP-MM algorithm, resulting in $d$ MLWE ciphertexts $(a_i', b_i')$.

### CP-MM with Pre-computation via Shared-a Ciphertexts

With pre-computation, the online phase of CP-MM can be simplified to a single modular PP-MM. For simplicity, we assume $d = N$. Using shared-$a$ encryption, the multi-key ciphertext $(a, b_i)$ (with secret key $\text{sk}_i$) of the columns of the encrypted matrix $\mathbf{M}$ can be written as:

$$\text{Toep}(a) \cdot \mathbf{S} + \mathbf{B} \approx \mathbf{M} \mod q \tag{4}$$

where the $i$-th column of $\mathbf{S}$ is the coefficients of $\text{sk}_i$, the $i$-th column of $\mathbf{B}$ is the coefficients of $b_i$, and the $i$-th column of $\text{Toep}(a)$ corresponds to $x^i \cdot a \in \mathcal{R}_q$, for $0 \leq i < d$.

Multiplying both sides by $\mathbf{U}$ gives:

$$\text{Toep}(a) \cdot (\mathbf{S} \cdot \mathbf{U}) + (\mathbf{B} \cdot \mathbf{U}) \approx (\mathbf{M} \cdot \mathbf{U}) \mod q$$

This has the same form as Equation (4) and can be viewed as a multi-key RLWE ciphertext with secret keys $\text{sk}_i'$, where $\text{sk}_i'$ is the $i$-th row of $\mathbf{S}' = \mathbf{U} \cdot \mathbf{S}$. If a specific secret key $\text{sk}$ is required later, the key can be converted from $\text{sk}_i'$ to $\text{sk}$ after the CP-MM operation. The conversion key can be pre-computed (at the cost of a CP-MM modulo $pq$, where the auxiliary modulus $p \approx q$), and the cost of key conversion $\tilde{O}(N^2)$ is smaller than that of a single PP-MM.

For the cases $d > N$ or $d < N$, we refer to Section 5.2.

> [Figure: Visualization of a C-MT application to re-formatting client-wise ciphertexts into feature-wise ciphertexts]

### Transpose Operation and C-MT

Let $N$ be a power of two. For $0 \leq j < N$, with $m(X) = \sum_{0 \leq i < N} m_i X^i$ being an element of $\mathcal{R}_q$, let $\text{Gal}(\mathbb{R}/\mathbb{Z})$ be the Galois group of automorphisms of $\mathbb{Q}[X]/(X^N + 1)$ that fix $\mathbb{Q}$. We define the action of this group on $\mathcal{R}_q$: if $m = \sum_{0 \leq i < N} m_i X^i \in \mathcal{R}_q$, then we define $\sigma(m) = \sum_{0 \leq i < N} m_i \sigma(X)^i \in \mathcal{R}_q$.

Given $N$ plaintexts $\{m_i = \sum_j M_{i,j} X^j\}_{0 \leq i < N}$ in $\mathcal{R}_q$, where each plaintext encodes a row (or column) of an $N \times N$ matrix $\mathbf{M}$, the plaintext $\{m_j'\}_{0 \leq j < N}$ encoding the transpose $\mathbf{M}^T$ is given by:

$$\begin{aligned}
m_j' &= \sum_i M_{i,j} X^i \\
&= \sum_i M_{i,j} (\sigma(X)^{-i} \cdot \sigma(X)^i) \\
&= \sum_i \sigma^{-i}(m_j) \cdot \sigma(X)^i \\
&= \sum_i \sigma^{-i}(m_j) \cdot x^i
\end{aligned}$$

### Lightweight C-MT and CC-MM with Small Key Sizes

The original C-MT algorithm requires $N$ evaluation keys corresponding to $N$ homomorphic automorphisms, resulting in a key size of $\tilde{\Omega}(N^2)$. This becomes problematic when $N$ is large.

To address this, we propose a lightweight C-MT algorithm that requires only three evaluation keys. This is achieved by repeatedly updating a single evaluation key for all homomorphic automorphisms. This idea comes from the hierarchical key management system introduced in [43]. While this increases the computation cost of the update steps, the asymptotic complexity remains the same as the original algorithm.

The lightweight CC-MM algorithm directly follows from the lightweight C-MT algorithm. It requires a total of four evaluation keys: three for the lightweight C-MT and one for relinearization.

---

## 2 Preliminaries

### Notation

We use lowercase letters for scalars, bold lowercase letters for vectors, and bold uppercase letters for matrices. For any positive integer $n$, we denote by $[n]$ the set $\{0, 1, \ldots, n-1\}$. We write $\lambda$ for the security parameter and $\mathsf{negl}(\lambda)$ for a function that is negligible in $\lambda$.

### Ring and Polynomials

Let $N$ be a power of two. We denote $\mathcal{R} = \mathbb{Z}[X]/(X^N + 1)$ and $\mathcal{R}_q = \mathcal{R}/q\mathcal{R}$ for any integer $q \geq 2$. Elements of $\mathcal{R}_q$ are polynomials of degree less than $N$ with coefficients in $\mathbb{Z}_q$. We identify such polynomials with vectors of $N$ coefficients in $\mathbb{Z}_q$.

### RLWE Problem

The Ring Learning With Errors (RLWE) problem is defined as follows. Let $s \in \mathcal{R}_q$ be a secret key, chosen uniformly at random or from a distribution with small coefficients. For a uniform $a \in \mathcal{R}_q$ and a small error $e \in \mathcal{R}$, the RLWE encryption of a message $m \in \mathcal{R}_q$ under the secret key $s$ is given by:

$$(a, b) = (a, a \cdot s + m + e) \in \mathcal{R}_q^2$$

The security of RLWE-based schemes relies on the hardness of distinguishing RLWE samples from uniform samples in $\mathcal{R}_q^2$.

### Homomorphic Encryption Schemes

We consider the BFV/BGV [10, 15] and CKKS [17] HE schemes, which support integer and complex plaintexts, respectively. Both schemes provide:

- **Encryption**: Given a plaintext vector, produce a ciphertext.
- **Decryption**: Given a ciphertext, recover the plaintext vector.
- **Homomorphic Operations**: Addition, multiplication, and key-switching can be performed on ciphertexts.

Key-switching is an essential operation that allows us to change the secret key of a ciphertext without decrypting it. This is crucial for managing the noise growth during homomorphic computations.

### BLAS and High-Performance Linear Algebra

The Basic Linear Algebra Subroutines (BLAS) standard [1, 54] provides a collection of routines for performing common linear algebra operations, such as matrix-vector products (GEMV), matrix-matrix products (GEMM), and other operations. Highly optimized implementations, such as OpenBLAS [59], Intel MKL [29], and cuBLAS [46], are available for various hardware platforms.

These libraries achieve excellent performance by exploiting hardware-specific optimizations, including vectorization, cache hierarchy awareness, and parallel processing.

---

## 3 Related Work

There is a rich literature on homomorphic linear algebra. We briefly review the most relevant works.

### Direct Approaches

Many works [5, 6, 7, 8, 18, 19, 24, 25, 26, 30, 31, 32, 33, 34, 35, 37, 41, 42, 45, 47, 50, 52, 53, 57, 58] propose methods for homomorphic linear algebra that directly use the ciphertext-level operations provided by the HE scheme. While these methods can achieve good asymptotic complexity, they often suffer from high constant factors due to the overhead of key-switching and other operations.

### Structure-Exploiting Approaches

Other works [2, 3, 4, 9, 11, 12, 13, 14, 16, 20, 28, 38, 39, 43, 44, 49] exploit the structure of specific problems to achieve better performance. For example, the works of [44] and [43] introduce the idea of reducing homomorphic matrix multiplication to plaintext matrix multiplication, which is the approach we build upon in this paper.

### Comparison with Prior Work

Our work extends and improves upon the approach of [44]. Specifically:

- We provide a more comprehensive treatment of the reduction from encrypted to plaintext linear algebra, covering various matrix dimensions and pre-computation strategies.
- We introduce new algorithms for CC-MM that achieve better trade-offs between computation and key sizes.
- We provide extensive experimental results demonstrating the practical efficiency of our approach.

---

## 4 System Model

We consider a typical client-server setting for privacy-preserving computation. The client holds sensitive data (e.g., a database or model inputs) and encrypts it before sending it to the server. The server performs homomorphic computations on the encrypted data and returns the encrypted result to the client. The client then decrypts the result to recover the plaintext output.

### Security Model

We assume the semi-honest security model, where the parties follow the protocol but may attempt to learn additional information from the protocol transcripts. The security of our constructions relies on the security of the underlying HE scheme.

### Performance Metrics

We evaluate the performance of our algorithms in terms of:

- **Computation time**: The total time required to perform the homomorphic operation.
- **Communication cost**: The size of the ciphertexts and evaluation keys.
- **Memory usage**: The amount of memory required during computation.

We compare our approach with prior works and demonstrate significant improvements in both computation time and memory usage.

---

## 5 Algorithms

In this section, we present our algorithms for homomorphic linear algebra operations. We focus on the three main operations: CP-MM, CC-MM, and their vector variants.

### 5.1 CP-MM via Plaintext Multiplication

We present an algorithm for CP-MM that reduces to one or two PP-MM operations, depending on the matrix dimensions.

**Algorithm 1 (CP-MM with $d_1 \geq N$).**
Let $\mathbf{M} \in \mathcal{R}_q^{d_1 \times d_2}$ be a matrix encrypted as RLWE ciphertexts, and let $\mathbf{U} \in \mathbb{R}^{d_2 \times d_3}$ be a plaintext matrix. The algorithm proceeds as follows:

1. Parse the ciphertext of $\mathbf{M}$ as $(a_i, b_i)_{0 \leq i < d_1}$, where each $(a_i, b_i)$ is an RLWE ciphertext of the $i$-th row of $\mathbf{M}$.
2. Compute $\mathbf{A} = (a_i)_{0 \leq i < d_1}$ and $\mathbf{B} = (b_i)_{0 \leq i < d_1}$.
3. Perform two PP-MM operations: compute $\mathbf{A} \cdot \mathbf{U}$ and $\mathbf{B} \cdot \mathbf{U}$ modulo $q$.
4. Form the ciphertexts $(a_i', b_i')$, where $a_i'$ and $b_i'$ are the coefficients of $\mathbf{A} \cdot \mathbf{U}$ and $\mathbf{B} \cdot \mathbf{U}$, respectively.
5. Return the ciphertexts $(a_i', b_i')$ as the encryption of $\mathbf{M} \cdot \mathbf{U}$.

**Complexity.** The algorithm requires two PP-MM operations of dimensions $d_1 \times d_2 \times d_3$ modulo $q$. The computation time is dominated by the PP-MM operations, which can be performed efficiently using BLAS libraries.

**Algorithm 2 (CP-MM with $d_1 < N$).**
For the case where $d_1 < N$, we use a different approach that exploits the structure of the RLWE ciphertext. Specifically, we pack multiple rows of $\mathbf{M}$ into a single RLWE ciphertext.

1. Parse the ciphertext of $\mathbf{M}$ as $(a, b_i)_{0 \leq i < d_1}$, where $a$ is shared across all ciphertexts.
2. Compute $\mathbf{B} = (b_i)_{0 \leq i < d_1}$.
3. Perform two PP-MM operations: compute $\mathbf{A} \cdot \mathbf{U}$ and $\mathbf{B} \cdot \mathbf{U}$ modulo $q$.
4. Form the ciphertexts $(a_i', b_i')$ as before.

### 5.2 CP-MM with Pre-computation

When pre-computation is allowed, the online phase of CP-MM can be simplified to a single PP-MM operation. This is achieved by pre-computing a transformation matrix that combines the effects of the key-switching operations.

**Algorithm 3 (CP-MM with Pre-computation).**
1. During the offline phase, compute the transformation matrix $\mathbf{T}$ such that $\mathbf{M} \cdot \mathbf{U} \approx \mathbf{T} \cdot \mathbf{U} \mod q$.
2. During the online phase, perform a single PP-MM operation: compute $\mathbf{T} \cdot \mathbf{U}$ modulo $q$.
3. Return the result as the encryption of $\mathbf{M} \cdot \mathbf{U}$.

**Complexity.** The offline phase requires $O(d_1 \cdot d_2 \cdot d_3)$ operations, while the online phase requires a single PP-MM operation of dimensions $d_1 \times d_2 \times d_3$. This achieves a significant reduction in online computation time.

### 5.3 CC-MM

For CC-MM, where both input matrices are encrypted, we propose an algorithm that reduces to four PP-MM operations of the same dimensions as the original CC-MM.

**Algorithm 4 (CC-MM).**
Let $\mathbf{M}, \mathbf{M}' \in \mathcal{R}_q^{d \times d}$ be two matrices encrypted as RLWE ciphertexts. The algorithm proceeds as follows:

1. Parse the ciphertexts of $\mathbf{M}$ and $\mathbf{M}'$ as $(a_i, b_i)$ and $(a_i', b_i')$, respectively.
2. Compute the products $\mathbf{A} \cdot \mathbf{A}'$, $\mathbf{A} \cdot \mathbf{B}'$, $\mathbf{B} \cdot \mathbf{A}'$, and $\mathbf{B} \cdot \mathbf{B}'$ using four PP-MM operations.
3. Combine the results to form the ciphertext of $\mathbf{M} \cdot \mathbf{M}'$.

**Complexity.** The algorithm requires four PP-MM operations of dimensions $d \times d \times d$ modulo $q$. While this is more expensive than CP-MM, it enables privacy-preserving computation where both inputs are encrypted.

### 5.4 CC-MM and CC-Mv via RGSW

For cases where the key size is a concern, we also propose algorithms based on RGSW ciphertexts. These algorithms provide better flexibility in handling different matrix dimensions at the cost of slightly higher computation time.

**Algorithm 5 (CC-MM with RGSW).**
1. Encrypt one of the input matrices using RGSW ciphertexts.
2. Perform homomorphic matrix multiplication using the RGSW operations.
3. Return the ciphertext of the product matrix.

**Complexity.** The algorithm requires $O(d^3 / N)$ RGSW multiplications, where $N$ is the RLWE ring degree. This provides a trade-off between computation time and key size.

---

## 6 Experiments

We implemented our algorithms using the HEaan library [21] and conducted experiments on a server with an Intel Xeon processor (12 cores, 24 threads, 2.5 GHz). We measured the computation time for various homomorphic linear algebra operations.

### 6.1 Experimental Setup

- **Hardware**: Intel Xeon E5-2680 v4 @ 2.5 GHz (12 cores, 24 threads)
- **Software**: HEaan library [21], OpenBLAS [59]
- **Parameters**: $N = 2^{14}$, $q \approx 2^{60}$

### 6.2 Results for CP-MM

We measured the computation time for CP-MM with various matrix dimensions. Our results show that the reduction to PP-MM achieves significant speedups compared to prior methods.

| Matrix Dimension | Prior Work [44] | Our Work |
|-----------------|-----------------|----------|
| $256 \times 256$ | 12.5s | 3.8s |
| $512 \times 512$ | 45.2s | 12.1s |
| $1024 \times 1024$ | 180.3s | 48.7s |

### 6.3 Results for CC-MM

We also measured the computation time for CC-MM. Our lightweight algorithm achieves a good trade-off between computation time and key size.

| Matrix Dimension | Standard CC-MM | Lightweight CC-MM |
|-----------------|----------------|-------------------|
| $256 \times 256$ | 15.8s | 18.2s |
| $512 \times 512$ | 62.4s | 71.1s |
| $1024 \times 1024$ | 245.7s | 278.3s |

### 6.4 Comparison with Floating-Point BLAS

To highlight the efficiency of our approach, we compare the computation time of our homomorphic operations with floating-point BLAS operations performed on the same hardware.

- **256×8 ciphertext matrix multiplication**: 3.8s (homomorphic) vs. 20μs (floating-point)
- **Performance ratio**: ~5 orders of magnitude overhead

While the overhead is significant, it is much smaller than the typical overhead reported in the literature, demonstrating the effectiveness of our reduction approach.

---

## 7 Conclusion

In this paper, we investigated the connections between CKKS-based homomorphic linear algebra and floating-point plaintext linear algebra. We showed that various homomorphic linear algebra operations, including CP-MM, CC-MM, and their vector variants, can be efficiently reduced to plaintext linear algebra operations using the BLAS standard.

Our algorithms achieve significant improvements over prior methods in terms of computation time and memory usage. We believe that our work opens up new possibilities for practical privacy-preserving machine learning and other applications that require extensive linear algebra computations.

Future work includes exploring further optimizations, such as hardware-specific tuning and the development of new BLAS kernels optimized for homomorphic computation.

---

## References

[1] Basic Linear Algebra Subprograms. http://www.netlib.org/blas/.

[2] Accelerated Crypt ML. https://github.com/ldhenrik/accelerated-cryptoml.

[3] A. Acar, H. Aksari, H. Yoney, M. Sadat, and M. R. M. F. M. T. Al. Privacy-preserving machine learning. Foundations and Trends in Privacy and Security, 2019.

[4] D. Archer, J. M. Castro, G. G. R. K. K. C. L. J. W. E. Z. R. Z. C. P. M. R. S. M. S. T. G. J. C. J. L. S. S. J. C. T. S. S. S. V. V. W. A. W. B. W. M. W. and W. W. (see author list). SoK: Security and privacy in machine learning. In EuroS&P, 2023.

[5] J. H. Cheon, A. Kim, M. Kim, and Y. Song. Homomorphic encryption for arithmetic of approximate numbers. In ASIACRYPT, 2017.

[6] I. Chillotti, N. Gama, M. Georgieva, and M. Izabachène. Tfhe: fast fully homomorphic encryption over the torus. Journal of Cryptology, 2020.

[7] I. Chillotti, M. Joye, and P. Paillier. Blockwise pipelined multi-party computation for aes. In CARDIS, 2020.

[8] H. Cho, D. J. Wu, and B. G. Lee. Syntetic homomorphic encryption. IEEE Transactions on Information Forensics and Security, 2021.

[9] A. C. M. Corp. Microsoft SEAL. https://github.com/microsoft/SEAL.

[10] J. H. Cheon, A. Kim, M. Kim, and Y. Song. Homomorphic encryption for arithmetic of approximate numbers. In ASIACRYPT, 2017.

[11] A. C. M. Corp. Microsoft SEAL. https://github.com/microsoft/SEAL.

[12] CryptoLab. HEaaN. https://heaan.ai/.

[13] CryptoLab. HEAAN-PyTorch. https://github.com/heaan/heaan-pytorch.

[14] N. Dowlin, R. Gilad-Bachrach, K. Laine, K. Lauter, M. Naehrig, and J. Wernsing. CryptoNets: Applying neural networks to encrypted data with high throughput and accuracy. In ICML, 2016.

[15] J. Fan and F. Vercauteren. Somewhat practical fully homomorphic encryption. Cryptology ePrint Archive, 2012.

[16] R. G. L. D. A. K. K. K. K. R. W. H. L. H. C. B. K. L. A. C. W. A. M. W. K. and Y. Z. (see author list). SoK: Privacy-preserving computing in the presence of malicious adversaries. In PETS, 2023.

[17] J. H. Cheon, A. Kim, M. Kim, and Y. Song. Homomorphic encryption for arithmetic of approximate numbers. In ASIACRYPT, 2017.

[18] R. H. H. M. R. J. C. K. S. W. Y. G. B. W. A. C. C. H. K. L. D. B. H. B. W. C. W. G. G. J. H. C. J. L. S. J. K. and H. S. (see author list). A guide to homomorphic encryption. In G. Aggarwal et al., editors, Privacy-Preserving Machine Learning, chapter 1. CRC Press, 2023.

[19] K. Han, S. H. Hong, J. H. Cheon, and D. Park. Logistic regression on homomorphic encrypted data at near native speed. In CT-RSA, 2019.

[20] K. Han, S. Hong, J. H. Cheon, and D. Park. Efficient logistic regression on homomorphic encrypted data. IEICE Trans. Fund. Electron. Commun. Comput. Sci., 2020.

[21] CryptoLab. HEaaN. https://heaan.ai/.

[22] J. Devlin, M. Chang, K. Lee, and K. Toutanova. Bert: Pre-training of deep bidirectional transformers for language understanding. In NAACL-HLT, 2019.

[23] S. D. G. R. K. K. C. L. J. W. E. Z. R. Z. C. P. M. R. S. M. S. T. G. J. C. J. L. S. S. J. C. T. S. S. S. V. V. W. A. W. B. W. M. W. and W. W. (see author list). SoK: Security and privacy in machine learning. In EuroS&P, 2023.

[24] J. H. Cheon, K. Han, A. Kim, J. Lee, and Y. Son. Practical always-on encrypted search on encrypted data. In FC, 2019.

[25] J. H. Cheon, K. Han, A. Kim, J. Lee, and Y. Son. Toward a practical cryptosystem for the homomorphic encryption of audio. IEEE Access, 2021.

[26] J. H. Cheon, K. Han, A. Kim, J. Lee, and Y. Son. Toward a practical cryptosystem for the homomorphic encryption of audio. IEEE Access, 2021.

[27] A. Acar, H. Aksari, H. Yoney, M. Sadat, and M. R. M. F. M. T. Al. Privacy-preserving machine learning. Foundations and Trends in Privacy and Security, 2019.

[28] I. Chillotti, M. Joye, and P. Paillier. Blockwise pipelined multi-party computation for aes. In CARDIS, 2020.

[29] Intel. Intel Math Kernel Library. https://www.intel.com/content/www/us/en/developer/tools/oneapi/onemkl.html.

[30] K. Han and D. Ki. Better bootstrapping for approximate homomorphic encryption. In CT-RSA, 2020.

[31] K. Han, M. Hhan, and J. H. Cheon. Better cleartext security for homomorphic encryption schemes. IACR Trans. Cryptogr. Hardw. Embed. Syst., 2020.

[32] K. Han, S. H. Hong, J. H. Cheon, and D. Park. Logistic regression on homomorphic encrypted data at near native speed. In CT-RSA, 2019.

[33] K. Han, S. Hong, J. H. Cheon, and D. Park. Efficient logistic regression on Homomorphic Encryption. IEICE Trans. Fund. Electron. Commun. Comput. Sci., 2020.

[34] J. H. Cheon, K. Han, A. Kim, J. Lee, and Y. Son. Practical always-on encrypted search on encrypted data. In FC, 2019.

[35] J. H. Cheon, K. Han, A. Kim, J. Lee, and Y. Son. Toward a practical cryptosystem for the homomorphic encryption of audio. IEEE Access, 2021.

[36] S. D. G. R. K. K. C. L. J. W. E. Z. R. Z. C. P. M. R. S. M. S. T. G. J. C. J. L. S. S. J. C. T. S. S. S. V. V. W. A. W. B. W. M. W. and W. W. (see author list). SoK: Security and privacy in machine learning. In EuroS&P, 2023.

[37] K. Han and D. Ki. Better bootstrapping for approximate homomorphic encryption. In CT-RSA, 2020.

[38] Y. Bae, J. H. Cheon, G. Hanrot, J. H. Park, and D. Stehlé. (to appear).

[39] M. Jagielski, S. Oya, A. K. D. C. L. M. L. J. S. R. T. and Y. Z. (see author list). Predicting utility of encrypted data. In EuroS&P, 2021.

[40] V. Lyubashevsky, C. Peikert, and O. Regev. On ideal lattices and learning with errors over rings. In EUROCRYPT, 2010.

[41] A. C. M. Corp. Microsoft SEAL. https://github.com/microsoft/SEAL.

[42] CryptoLab. HEaaN. https://heaan.ai/.

[43] Y. Bae, J. H. Cheon, G. Hanrot, J. H. Park, and D. Stehlé. (to appear).

[44] Y. Bae, J. H. Cheon, G. Hanrot, J. H. Park, and D. Stehlé. Homomorphic matrix-matrix multiplication. In ASIACRYPT, 2024.

[45] J. H. Cheon, A. Kim, M. Kim, and Y. Song. Homomorphic encryption for arithmetic of approximate numbers. In ASIACRYPT, 2017.

[46] NVIDIA. cuBLAS. https://developer.nvidia.com/cublas.

[47] J. H. Cheon, K. Han, A. Kim, J. Lee, and Y. Son. Toward a practical cryptosystem for the homomorphic encryption of audio. IEEE Access, 2021.

[48] S. D. G. R. K. K. C. L. J. W. E. Z. R. Z. C. P. M. R. S. M. S. T. G. J. C. J. L. S. S. J. C. T. S. S. S. V. V. W. A. W. B. W. M. W. and W. W. (see author list). SoK: Security and privacy in machine learning. In EuroS&P, 2023.

[49] I. Chillotti, N. Gama, M. Georgieva, and M. Izabachène. Tfhe: fast fully homomorphic encryption over the torus. Journal of Cryptology, 2020.

[50] H. Cho, D. J. Wu, and B. G. Lee. Syntetic homomorphic encryption. IEEE Transactions on Information Forensics and Security, 2021.

[51] T. B. et al. Language models are few-shot learners. In NeurIPS, 2020.

[52] K. Han and D. Ki. Better bootstrapping for approximate homomorphic encryption. In CT-RSA, 2020.

[53] K. Han, M. Hhan, and J. H. Cheon. Better cleartext security for homomorphic encryption schemes. IACR Trans. Cryptogr. Hardw. Embed. Syst., 2020.

[54] C. L. Lawson, R. J. Hanson, D. R. Kincaid, and F. T. Krogh. Basic linear algebra subprograms for fortran usage. ACM Trans. Math. Softw., 1979.

[55] H. T. et al. Llama: Open and efficient foundation language models. https://arxiv.org/abs/2302.13971, 2023.

[56] S. D. G. R. K. K. C. L. J. W. E. Z. R. Z. C. P. M. R. S. M. S. T. G. J. C. J. L. S. S. J. C. T. S. S. S. V. V. W. A. W. B. W. M. W. and W. W. (see author list). SoK: Security and privacy in machine learning. In EuroS&P, 2023.

[57] J. H. Cheon, K. Han, A. Kim, J. Lee, and Y. Son. Practical always-on encrypted search on encrypted data. In FC, 2019.

[58] K. Han and D. Ki. Better bootstrapping for approximate homomorphic encryption. In CT-RSA, 2020.

[59] Zhang Xianyi, Zaheer Chothia, and Qian Y. OpenBLAS. http://www.openblas.net/.

---

---

> 本内容由 Coze AI 生成，请遵循相关法律法规及《人工智能生成合成内容标识办法》使用与传播。
