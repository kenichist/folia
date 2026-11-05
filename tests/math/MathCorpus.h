// Fixed formula corpus shared by math_tests (constructs + perf) and
// math_gallery. Covers every construct of the declared subset.
#pragma once

struct MathCorpusEntry {
    const char *label;
    const char *tex;
};

static const MathCorpusEntry kMathCorpus[] = {
    {"scripts & primes", R"(x_i^2 + y'_j + f''(x) + e^{-x^2} + a_{n+1}^{(k)})"},
    {"quadratic formula", R"(x = \frac{-b \pm \sqrt{b^2 - 4ac}}{2a})"},
    {"fractions & binomials", R"(\frac{1}{2} + \dfrac{a}{b} + \tfrac{3}{4} + \binom{n}{k} = \frac{\frac{a}{b}}{c})"},
    {"radicals", R"(\sqrt{2}\,\sqrt{x^2+y^2}\,\sqrt[3]{\frac{a}{b}}\,\sqrt[n]{1+x})"},
    {"greek lower", R"(\alpha\beta\gamma\delta\epsilon\varepsilon\zeta\eta\theta\vartheta\iota\kappa\lambda\mu\nu\xi\pi\varpi\rho\varrho\sigma\varsigma\tau\upsilon\phi\varphi\chi\psi\omega)"},
    {"greek upper", R"(\Gamma\Delta\Theta\Lambda\Xi\Pi\Sigma\Upsilon\Phi\Psi\Omega)"},
    {"operators & relations", R"(\infty\ \partial\ \nabla\ a\cdot b\times c \pm d \mp e \leq \geq \neq \approx \equiv \sim \propto)"},
    {"arrows & sets", R"(\to \rightarrow \Rightarrow \Leftrightarrow \mapsto \quad x \in A \notin B \subset C \subseteq D \cup E \cap F \quad \forall \exists)"},
    {"dots", R"(1, 2, \ldots, n \quad a_1 + \cdots + a_n \quad \vdots \quad \ddots)"},
    {"sum & product", R"(\sum_{i=1}^{n} i = \frac{n(n+1)}{2}, \quad \prod_{p} \left(1 - \frac{1}{p^s}\right)^{-1})"},
    {"integrals", R"(\int_0^1 x^2\,dx \quad \iint_D f\,dA \quad \oint_C \vec{F}\cdot d\vec{r})"},
    {"big set operators", R"(\bigcup_{i=1}^n A_i = \bigcap_{j \in J} B_j)"},
    {"limits & functions", R"(\lim_{x \to 0} \frac{\sin x}{x} = 1, \quad \max_{i} a_i, \quad \log_2 n, \quad \det A, \quad \operatorname{tr} M)"},
    {"delimiters", R"(( [ \{ | \| \langle x \rangle \| | \} ] ) \quad \lfloor x \rfloor \lceil y \rceil)"},
    {"\\left \\right", R"(\left( \frac{a}{b} \right) \left[ \sum_{i=1}^n x_i \right] \left\{ \frac{\frac{1}{2}}{3} \right. \left| \int_0^1 f \right| \left\langle \psi \middle| \phi \right\rangle)"},
    {"\\big sizes", R"(\big( \Big( \bigg( \Bigg( x \Bigg) \bigg) \Big) \big) \quad \bigl[ \Bigl\{ y \Bigr\} \bigr])"},
    {"matrices", R"(\begin{matrix} a & b \\ c & d \end{matrix} \quad \begin{pmatrix} 1 & 0 \\ 0 & 1 \end{pmatrix} \quad \begin{bmatrix} x_{11} & x_{12} \\ x_{21} & x_{22} \end{bmatrix})"},
    {"more matrices", R"(\begin{Bmatrix} a \\ b \end{Bmatrix} \begin{vmatrix} a & b \\ c & d \end{vmatrix} = ad - bc \quad \begin{Vmatrix} \vec{v} \end{Vmatrix})"},
    {"cases", R"(|x| = \begin{cases} x & \text{if } x \geq 0 \\ -x & \text{otherwise} \end{cases})"},
    {"aligned", R"(\begin{aligned} f(x) &= (x+1)^2 \\ &= x^2 + 2x + 1 \\ g(x) &\leq \frac{1}{x} \end{aligned})"},
    {"gathered", R"(\begin{gathered} a = b + c \\ \frac{d}{e} = f \end{gathered})"},
    {"array", R"(\begin{array}{c|cc} & x & y \\ \hline 1 & a & b \\ 2 & c & d \end{array})"},
    {"accents", R"(\hat{a}\ \bar{x}\ \vec{v}\ \tilde{n}\ \dot{y}\ \ddot{z}\ \overline{AB}\ \underline{xy}\ \widehat{xyz}\ \widetilde{abc})"},
    {"braces", R"(\overbrace{a + b + c}^{n} \quad \underbrace{x + y + z}_{3\text{ terms}})"},
    {"alphabets", R"(\mathrm{d}x\; \mathbf{v}\; \mathit{it}\; \mathsf{S}\; \mathtt{tt}\; \mathbb{R}^n\; \mathcal{L}\; \mathfrak{g}\; \text{text here})"},
    {"spacing", R"(a\,b\:c\;d\!e\quad f\qquad g~h)"},
    {"styles", R"(\displaystyle\sum_i x_i \quad \textstyle\sum_i x_i \quad \scriptstyle x + y)"},
    {"Euler", R"(e^{i\pi} + 1 = 0)"},
    {"Gaussian integral", R"(\int_{-\infty}^{\infty} e^{-x^2}\,dx = \sqrt{\pi})"},
    {"Faraday", R"(\nabla \times \vec{E} = -\frac{\partial \vec{B}}{\partial t})"},
    {"Taylor series", R"(f(x) = \sum_{n=0}^{\infty} \frac{f^{(n)}(a)}{n!}(x-a)^n)"},
    {"continued fraction", R"(x = a_0 + \cfrac{1}{a_1 + \cfrac{1}{a_2 + \cdots}})"},
    {"nested delimiters", R"(\left( \left[ \left\{ \frac{x^{y^z}}{\sqrt{1 + \frac{1}{x}}} \right\} \right] \right))"},
    {"negation & stacks", R"(a \not= b,\; x \not\in A,\; \neg p \quad \overset{!}{=} \quad \underset{n\to\infty}{\longrightarrow} \quad \stackrel{\text{def}}{=})"},
};

static const int kMathCorpusSize = int(sizeof(kMathCorpus) / sizeof(kMathCorpus[0]));
