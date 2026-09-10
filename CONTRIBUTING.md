# Contributing to rtss-mailbox-kmd

Hi there!
We're thrilled that you'd like to contribute to this project.
Your help is essential for keeping this project great and for making it better.

## Branching Strategy

In general, contributors should develop on branches based off of `main` and pull requests should be made against `main`.

The `rtss-mailbox-kernel.le.0.0` branch is a product release branch maintained by Qualcomm.
Direct contributions to it are not accepted — fixes land on `main` first.

## Submitting a pull request

1. Please read our [code of conduct](CODE-OF-CONDUCT.md) and [license](LICENSE.txt).

2. [Fork](https://github.com/qualcomm-linux/rtss-mailbox-kmd/fork) and clone the repository.

    ```bash
    git clone https://github.com/<username>/rtss-mailbox-kmd.git
    ```

    > **Windows users:** disable automatic line-ending conversion before cloning to
    > preserve LF line endings enforced by `.gitattributes`:
    > ```bash
    > git config core.autocrlf false
    > ```

3. Create a new branch based on `main`:

    ```bash
    git checkout -b <my-branch-name> main
    ```

4. Create an upstream `remote` to make it easier to keep your branches up-to-date:

    ```bash
    git remote add upstream https://github.com/qualcomm-linux/rtss-mailbox-kmd.git
    ```

5. Make your changes and make sure existing functionality is not broken.

6. Commit your changes using the [DCO](https://developercertificate.org/). You can attest to the DCO by commiting with the **-s** or **--signoff** options or manually adding the "Signed-off-by":

    ```bash
    git commit -s -m "Really useful commit message"
    ```

7. After committing your changes on the topic branch, sync it with the upstream branch:

    ```bash
    git pull --rebase upstream main
    ```

8. Push to your fork.

    ```bash
    git push -u origin <my-branch-name>
    ```

    The `-u` is shorthand for `--set-upstream`. This will set up the tracking reference so subsequent runs of `git push` or `git pull` can omit the remote and branch.

9. [Submit a pull request](https://github.com/qualcomm-linux/rtss-mailbox-kmd/pulls) from your branch to `main`.

10. Pat yourself on the back and wait for your pull request to be reviewed.

## Security Analysis of Pull Requests

To maintain the security and integrity of this project, all pull requests from external contributors are automatically scanned using [Semgrep](https://github.com/semgrep/semgrep) to detect insecure coding patterns and potential security flaws.

**Static Analysis with Semgrep:**  We use Semgrep to perform lightweight, fast static analysis on every PR. This helps identify risky code patterns and logic flaws early in the development process.

**Contributor Responsibility:** If any issues are flagged, contributors are expected to resolve them before the PR can be merged.

**Continuous Improvement:** Our Semgrep ruleset evolves over time to reflect best practices and emerging security concerns.

By submitting a PR, you agree to participate in this process and help us keep the project secure for everyone.

Here are a few things you can do that will increase the likelihood of your pull request being accepted:

- Follow the Linux kernel C coding style.
- Keep your change as focused as possible.
  If you want to make multiple independent changes, please consider submitting them as separate pull requests.
- Write a [good commit message](https://tbaggery.com/2008/04/19/a-note-about-git-commit-messages.html).
- It's a good idea to arrange a discussion with other developers to ensure there is consensus on large features, architecture changes, and other core code changes. PR reviews will go much faster when there are no surprises.

## Build and Test

Build the DLKM against your kernel source:

```bash
export KERNEL_SRC=<path to kernel build directory>
make ARCH=arm64 CROSS_COMPILE=aarch64-qcom-linux-
```

### Kernel version compatibility

When an upstream kernel API changes signature between versions, add a compat
wrapper to `include/rtss_mailbox_compat.h` rather than changing the driver
source directly. Follow the existing pattern:

1. Add a `#if LINUX_VERSION_CODE >= KERNEL_VERSION(x, y, 0)` block.
2. Define an `rtssmb_*` inline wrapper or macro pair for both sides.
3. Reference the upstream commit that introduced the change in the comment.
4. Use the wrapper in `osal/rtss_mailbox.c` — never call the kernel API directly.
5. Update the compatibility table in `README.md`.

See `rtssmb_eventfd_signal`, `rtssmb_class_create`, and
`RTSSMB_REMOVE_RETURN_TYPE` in the header for worked examples.
