"""Control response ownership for the RT dual-disk progress fixture."""


def release_rt_response(stream, line, draining):
    """Return whether the controller has submitted a whole-gate drain."""
    # Once drain is queued, it owns every held reply up to its ACK. Later
    # notifications may already be in the pipe; releasing them again closes
    # the strict backend because drain has already retired their identities.
    if draining:
        return True
    if b'command=0 ' in line:
        stream.write(b'drain\n')
        return True
    identity = int(line.split()[0].split(b'=')[1])
    stream.write(f'release {identity}\n'.encode())
    return draining


def rt_round_complete(progressed, observed, draining):
    # Pipe ordering is per backend, not across the guest and both backends.
    # Drain ACK retires all old held notifications before a new hold epoch.
    return progressed and not draining and all((disk, event) in observed
                              for disk in ('a', 'b') for event in ('READ', 'WRITE', 'FLUSH'))
