import time
from pymongo.errors import PyMongoError, ConnectionFailure, OperationFailure, ExecutionTimeout

# TODO(DRIVERS-1401): Use error labels instead of checking against an allow list of error codes.
retryable_codes = [
    # From the SDAM spec, the "node is shutting down" codes.
    11600,  # InterruptedAtShutdown
    91,  # ShutdownInProgress
    # From the SDAM spec, the "not primary" and "node is recovering" error codes.
    10058,  # LegacyNotPrimary <=3.2 "not primary" error code
    10107,  # NotWritablePrimary
    13435,  # NotPrimaryNoSecondaryOk
    11602,  # InterruptedDueToReplStateChange
    13436,  # NotPrimaryOrSecondary
    189,  # PrimarySteppedDown
    # From the retryable reads/writes spec.
    7,  # HostNotFound
    6,  # HostUnreachable
    89,  # NetworkTimeout
    9001,  # SocketException
    262,  # ExceededTimeLimit
]


def is_retryable_error(exc):
    if isinstance(exc, ConnectionFailure):
        return True
    if exc.has_error_label("RetryableWriteError"):
        return True
    if isinstance(exc, OperationFailure) and exc.code in retryable_codes:
        return True
    return False


def with_naive_retry(func, timeout=100):
    """
    Retry execution of a provided function naively for up to `timeout` seconds.

    This method is only suitable for reads or other idempotent operations. It is not suitable for
    retrying non-idempotent operations (most writes).

    :param func: The function to execute
    :param timeout: The maximum amount of time to retry execution
    """

    last_exc = None
    start = time.monotonic()
    while time.monotonic() - start < timeout:
        try:
            return func()
        except PyMongoError as exc:
            last_exc = exc
            if not is_retryable_error(exc):
                raise
        time.sleep(0.1)

    raise ExecutionTimeout(
        f"Operation exceeded time limit after {timeout} seconds, last error: {last_exc}")
