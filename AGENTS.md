# Workspace service source

Keep this service small. The C++ daemon owns local authorization, mount/process
lifetime and request journaling. The Python bridge reuses the pinned manager.
Preserve its source/owner/checkpoint rules; do not reimplement Git storage.

Use disposable repositories for tests. Never aim tests at Paperclip's existing
workspaces, credentials, task windows or live host policy. Changes to deployment,
runtime permissions or supported command profiles need explicit current scope.

Do not delegate this implementation to Paperclip. Preserve its existing owners.
Keep secrets and runtime records out of source. Record limitations truthfully;
passing service tests does not establish Paperclip/provider acceptance.
