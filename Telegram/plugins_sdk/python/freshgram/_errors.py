class FreshgramError(Exception):
    def __init__(self, code, message=""):
        super().__init__(code + (": " + message if message else ""))
        self.code = code
        self.message = message


class Denied(FreshgramError):
    pass


class Invalid(FreshgramError):
    pass


class NotFound(FreshgramError):
    pass


class QuotaExceeded(FreshgramError):
    pass


class LimitExceeded(FreshgramError):
    pass


class Unavailable(FreshgramError):
    pass


class Failed(FreshgramError):
    pass


_ERRORS = {
    "denied": Denied,
    "invalid": Invalid,
    "not_found": NotFound,
    "quota": QuotaExceeded,
    "limit": LimitExceeded,
    "unavailable": Unavailable,
}


def from_response(response):
    """The value of a response, or the error it carries raised."""
    if not isinstance(response, dict):
        raise Failed("failed", "a malformed response")
    if response.get("ok"):
        return response.get("v")
    code = response.get("e", "failed")
    raise _ERRORS.get(code, Failed)(code, response.get("m", ""))
