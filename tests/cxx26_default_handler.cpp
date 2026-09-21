namespace {

int checkedIndex(int i, int n) pre(i >= 0 && i < n) { return i; }

} // namespace

int main(int argc, char**) {
    return checkedIndex(argc + 4, 2) == argc + 4 ? 0 : 1;
}
