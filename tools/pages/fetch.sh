#!/bin/sh
# Downloads the layout test corpus.
#
# The pages themselves are not committed -- they are megabytes of somebody
# else's markup and they go stale -- but the list is, so the corpus can be
# rebuilt. Each page is fetched with the browser's own User-Agent, because a
# site that recognises curl serves different markup than it serves hawkOS,
# and the whole point is to test what the browser will actually receive.
set -e
cd "$(dirname "$0")"

UA='hawkOS/0.6'
get() {
    name="$1"; url="$2"
    if curl -sL -A "$UA" -H 'Accept: text/html,text/plain,*/*' \
            --max-time 30 "$url" -o "$name.html"; then
        echo "  $name.html  $(wc -c < "$name.html") bytes"
    else
        echo "  $name.html  FAILED"
    fi
}

get google    'https://www.google.com/'
get youtube   'https://www.youtube.com/'
get wikipedia 'https://en.wikipedia.org/wiki/Operating_system'
get example   'https://example.com/'
get ddg       'https://lite.duckduckgo.com/lite/?q=hobby+operating+system'
get hn        'https://news.ycombinator.com/'
get bbc       'https://www.bbc.com/news'
