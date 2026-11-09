/* with -fwrapv, x*x<0 can be true (wraps); without it clang may fold it to 0 */
int f(int x) { return x * x < 0; }
/* fp-contract: a*b+c must stay a separate mul and add */
double g(double a, double b, double c) { return a * b + c; }
