(function () {
  "use strict";

  var root = document.documentElement;
  var body = document.body;
  var THEME_KEY = "dsco-math-wiki-theme";

  function precedingResourceId(heading) {
    var node = heading.previousElementSibling;
    if (!node) return heading.id;
    if (node.matches("a[id]")) return node.id;
    var anchor = node.querySelector && node.querySelector(":scope > a[id]");
    return anchor ? anchor.id : heading.id;
  }

  function absoluteFragment(id) {
    return new URL("#" + id, window.location.href).href;
  }

  function setTheme(theme) {
    root.dataset.theme = theme;
    var button = document.querySelector("[data-theme-toggle]");
    if (button) {
      button.textContent = theme === "dark" ? "☀" : "☾";
      button.setAttribute(
        "aria-label",
        theme === "dark" ? "Use light theme" : "Use dark theme"
      );
    }
  }

  var savedTheme = null;
  try {
    savedTheme = window.localStorage.getItem(THEME_KEY);
  } catch (_) {
    savedTheme = null;
  }
  setTheme(
    savedTheme ||
      (window.matchMedia("(prefers-color-scheme: dark)").matches
        ? "dark"
        : "light")
  );

  var originalNodes = Array.prototype.slice.call(body.childNodes);
  var content = document.createElement("main");
  content.className = "wiki-content";
  content.id = "wiki-content";
  originalNodes.forEach(function (node) {
    content.appendChild(node);
  });

  Array.prototype.slice
    .call(content.querySelectorAll("p > a[id]"))
    .forEach(function (anchor) {
      if (anchor.parentElement.textContent.trim() === "") {
        anchor.parentElement.classList.add("resource-marker");
      }
    });

  var conceptHeadings = Array.prototype.slice.call(
    content.querySelectorAll("h3")
  );
  var entries = conceptHeadings.map(function (heading) {
    var id = precedingResourceId(heading);
    var text = "";
    var node = heading.nextElementSibling;
    while (node && !node.matches("h2, h3")) {
      text += " " + (node.textContent || "");
      node = node.nextElementSibling;
    }
    return {
      heading: heading,
      id: id,
      title: heading.textContent.trim(),
      text: text.toLowerCase()
    };
  });
  var entryById = new Map(
    entries.map(function (entry) {
      return [entry.id, entry];
    })
  );

  conceptHeadings.forEach(function (heading) {
    var id = precedingResourceId(heading);
    var button = document.createElement("button");
    button.className = "permalink-button";
    button.type = "button";
    button.textContent = "#";
    button.title = "Copy concept link";
    button.setAttribute("aria-label", "Copy link to " + heading.textContent);
    button.dataset.copyFragment = id;
    heading.appendChild(button);
  });

  var equations = Array.prototype.slice.call(
    content.querySelectorAll(".equation-card[id]")
  );
  equations.forEach(function (equation) {
    var button = document.createElement("button");
    button.className = "equation-link";
    button.type = "button";
    button.textContent = "#";
    button.title = "Copy equation link";
    button.setAttribute("aria-label", "Copy link to " + equation.id);
    button.dataset.copyFragment = equation.id;
    equation.appendChild(button);
  });

  var progress = document.createElement("div");
  progress.className = "reading-progress";
  progress.setAttribute("aria-hidden", "true");

  var toolbar = document.createElement("header");
  toolbar.className = "wiki-toolbar";
  toolbar.innerHTML =
    '<div class="wiki-toolbar__inner">' +
    '<a class="wiki-mark" href="#math-wiki" aria-label="Wiki home">∑</a>' +
    '<div class="wiki-toolbar__title">The Math Behind the Glossary</div>' +
    '<button class="icon-button" type="button" data-search-focus aria-label="Search concepts">⌕</button>' +
    '<button class="icon-button" type="button" data-theme-toggle aria-label="Toggle theme">☾</button>' +
    "</div>";

  var sidebar = document.createElement("aside");
  sidebar.className = "wiki-sidebar";
  sidebar.setAttribute("aria-label", "Wiki navigation");

  var sidebarIntro = document.createElement("div");
  sidebarIntro.innerHTML =
    '<p class="wiki-sidebar__eyebrow">Self-describing mathematics</p>' +
    "<h2>Concept navigator</h2>" +
    '<p class="wiki-stats">' +
    entries.length +
    " concepts · " +
    equations.length +
    " addressable equations</p>";

  var search = document.createElement("div");
  search.className = "wiki-search";
  search.innerHTML =
    '<label class="sr-only" for="wiki-search-input">Search concepts and equations</label>' +
    '<input id="wiki-search-input" type="search" autocomplete="off" ' +
    'placeholder="Search the graph…" aria-controls="wiki-search-results" aria-expanded="false">' +
    "<kbd>/</kbd>";
  var searchInput = search.querySelector("input");

  var searchResults = document.createElement("div");
  searchResults.className = "search-results";
  searchResults.id = "wiki-search-results";
  searchResults.setAttribute("role", "listbox");
  searchResults.dataset.open = "false";

  var toc = document.createElement("nav");
  toc.className = "wiki-toc";
  toc.setAttribute("aria-label", "Subject areas");
  var sectionHeadings = Array.prototype.slice.call(
    content.querySelectorAll("h2")
  );
  sectionHeadings.forEach(function (heading) {
    var link = document.createElement("a");
    link.href = "#" + precedingResourceId(heading);
    link.textContent = heading.textContent.replace(/^\d+\.\s*/, "");
    link.dataset.sectionTarget = precedingResourceId(heading);
    toc.appendChild(link);
  });

  var keyboardLegend = document.createElement("div");
  keyboardLegend.className = "keyboard-legend";
  keyboardLegend.innerHTML =
    "<kbd>/</kbd> search · <kbd>j</kbd>/<kbd>k</kbd> concepts · " +
    "<kbd>g</kbd> graph";

  sidebar.appendChild(sidebarIntro);
  sidebar.appendChild(search);
  sidebar.appendChild(searchResults);
  sidebar.appendChild(toc);
  sidebar.appendChild(keyboardLegend);

  var shell = document.createElement("div");
  shell.className = "wiki-shell";
  shell.appendChild(sidebar);
  shell.appendChild(content);

  var backToTop = document.createElement("button");
  backToTop.className = "back-to-top";
  backToTop.type = "button";
  backToTop.textContent = "↑";
  backToTop.setAttribute("aria-label", "Back to top");
  backToTop.dataset.visible = "false";

  var toast = document.createElement("div");
  toast.className = "copy-toast";
  toast.setAttribute("role", "status");
  toast.setAttribute("aria-live", "polite");
  toast.textContent = "Deep link copied";

  body.replaceChildren(progress, toolbar, shell, backToTop, toast);
  setTheme(root.dataset.theme);

  function showToast(message) {
    toast.textContent = message;
    toast.dataset.visible = "true";
    window.clearTimeout(showToast.timer);
    showToast.timer = window.setTimeout(function () {
      toast.dataset.visible = "false";
    }, 1500);
  }

  function copyText(text) {
    if (navigator.clipboard && window.isSecureContext) {
      return navigator.clipboard.writeText(text);
    }
    return new Promise(function (resolve, reject) {
      var area = document.createElement("textarea");
      area.value = text;
      area.style.position = "fixed";
      area.style.opacity = "0";
      body.appendChild(area);
      area.select();
      try {
        document.execCommand("copy") ? resolve() : reject(new Error("copy"));
      } catch (error) {
        reject(error);
      } finally {
        area.remove();
      }
    });
  }

  body.addEventListener("click", function (event) {
    var copyButton = event.target.closest("[data-copy-fragment]");
    if (copyButton) {
      copyText(absoluteFragment(copyButton.dataset.copyFragment))
        .then(function () {
          showToast("Deep link copied");
        })
        .catch(function () {
          window.location.hash = copyButton.dataset.copyFragment;
          showToast("Link opened");
        });
      return;
    }
    if (event.target.closest("[data-theme-toggle]")) {
      var next = root.dataset.theme === "dark" ? "light" : "dark";
      setTheme(next);
      try {
        window.localStorage.setItem(THEME_KEY, next);
      } catch (_) {
        return;
      }
    }
    if (event.target.closest("[data-search-focus]")) {
      searchInput.focus();
    }
  });

  backToTop.addEventListener("click", function () {
    window.scrollTo({ top: 0, behavior: "smooth" });
  });

  function normalize(value) {
    return value.toLowerCase().replace(/\s+/g, " ").trim();
  }

  function score(entry, query) {
    var title = entry.title.toLowerCase();
    if (title === query) return 100;
    if (title.indexOf(query) === 0) return 80;
    if (title.indexOf(query) !== -1) return 60;
    if (entry.text.indexOf(query) !== -1) return 20;
    var terms = query.split(" ");
    return terms.every(function (term) {
      return title.indexOf(term) !== -1 || entry.text.indexOf(term) !== -1;
    })
      ? 10
      : 0;
  }

  function closeSearch() {
    searchResults.dataset.open = "false";
    searchInput.setAttribute("aria-expanded", "false");
  }

  function renderSearch() {
    var query = normalize(searchInput.value);
    searchResults.replaceChildren();
    if (!query) {
      closeSearch();
      return;
    }
    var matches = entries
      .map(function (entry) {
        return { entry: entry, rank: score(entry, query) };
      })
      .filter(function (result) {
        return result.rank > 0;
      })
      .sort(function (a, b) {
        return b.rank - a.rank || a.entry.title.localeCompare(b.entry.title);
      })
      .slice(0, 12);

    if (!matches.length) {
      var empty = document.createElement("div");
      empty.className = "search-result";
      empty.textContent = "No matching concept";
      searchResults.appendChild(empty);
    } else {
      matches.forEach(function (result, index) {
        var link = document.createElement("a");
        link.className = "search-result";
        link.href = "#" + result.entry.id;
        link.setAttribute("role", "option");
        link.setAttribute("aria-selected", index === 0 ? "true" : "false");
        link.textContent = result.entry.title;
        link.addEventListener("click", function () {
          searchInput.value = "";
          closeSearch();
        });
        searchResults.appendChild(link);
      });
    }
    searchResults.dataset.open = "true";
    searchInput.setAttribute("aria-expanded", "true");
  }

  searchInput.addEventListener("input", renderSearch);
  searchInput.addEventListener("keydown", function (event) {
    var options = Array.prototype.slice.call(
      searchResults.querySelectorAll('[role="option"]')
    );
    var selected = options.findIndex(function (option) {
      return option.getAttribute("aria-selected") === "true";
    });
    if (event.key === "Escape") {
      searchInput.value = "";
      closeSearch();
      searchInput.blur();
    } else if (event.key === "ArrowDown" && options.length) {
      event.preventDefault();
      options[Math.max(0, selected)].setAttribute("aria-selected", "false");
      selected = (selected + 1) % options.length;
      options[selected].setAttribute("aria-selected", "true");
    } else if (event.key === "ArrowUp" && options.length) {
      event.preventDefault();
      options[Math.max(0, selected)].setAttribute("aria-selected", "false");
      selected = (selected - 1 + options.length) % options.length;
      options[selected].setAttribute("aria-selected", "true");
    } else if (event.key === "Enter" && options.length) {
      event.preventDefault();
      window.location.hash = options[Math.max(0, selected)].hash;
      searchInput.value = "";
      closeSearch();
    }
  });

  function currentEntryIndex() {
    var threshold = window.scrollY + 130;
    var index = 0;
    entries.forEach(function (entry, candidate) {
      var anchor = document.getElementById(entry.id);
      if (anchor && anchor.offsetTop <= threshold) index = candidate;
    });
    return index;
  }

  document.addEventListener("keydown", function (event) {
    var editable =
      event.target.matches("input, textarea, select") ||
      event.target.isContentEditable;
    if (event.key === "/" && !editable) {
      event.preventDefault();
      searchInput.focus();
      return;
    }
    if (editable || event.metaKey || event.ctrlKey || event.altKey) return;
    if (event.key === "g") {
      window.location.hash = "wiki-concept-graph";
    } else if (event.key === "j" || event.key === "k") {
      var delta = event.key === "j" ? 1 : -1;
      var index = Math.max(
        0,
        Math.min(entries.length - 1, currentEntryIndex() + delta)
      );
      window.location.hash = entries[index].id;
    }
  });

  var headingAndLinks = Array.prototype.slice.call(
    content.querySelectorAll('h2, h3, a[href^="#wiki-"]')
  );
  var currentOwner = null;
  var incoming = new Map();
  headingAndLinks.forEach(function (node) {
    if (node.matches("h2, h3")) {
      currentOwner = {
        id: precedingResourceId(node),
        title: node.textContent.replace("#", "").trim()
      };
      return;
    }
    if (node.closest(".math-hypermedia") || !currentOwner) return;
    var targetId = node.getAttribute("href").slice(1);
    if (!entryById.has(targetId) || targetId === currentOwner.id) return;
    if (!incoming.has(targetId)) incoming.set(targetId, new Map());
    incoming.get(targetId).set(currentOwner.id, currentOwner.title);
  });

  entries.forEach(function (entry) {
    var sources = incoming.get(entry.id);
    if (!sources || !sources.size) return;
    var details = document.createElement("details");
    details.className = "backlinks";
    var summary = document.createElement("summary");
    summary.textContent =
      sources.size + (sources.size === 1 ? " backlink" : " backlinks");
    var list = document.createElement("ul");
    list.className = "backlinks__list";
    sources.forEach(function (title, id) {
      var item = document.createElement("li");
      var link = document.createElement("a");
      link.href = "#" + id;
      link.textContent = title;
      item.appendChild(link);
      list.appendChild(item);
    });
    details.appendChild(summary);
    details.appendChild(list);
    var nav = entry.heading.nextElementSibling;
    if (nav && nav.matches(".math-hypermedia")) nav.after(details);
  });

  var tocLinks = Array.prototype.slice.call(
    toc.querySelectorAll("[data-section-target]")
  );
  if ("IntersectionObserver" in window) {
    var observer = new IntersectionObserver(
      function (observations) {
        observations.forEach(function (observation) {
          if (!observation.isIntersecting) return;
          var id = precedingResourceId(observation.target);
          tocLinks.forEach(function (link) {
            if (link.dataset.sectionTarget === id) {
              link.setAttribute("aria-current", "location");
            } else {
              link.removeAttribute("aria-current");
            }
          });
        });
      },
      { rootMargin: "-18% 0px -72% 0px" }
    );
    sectionHeadings.forEach(function (heading) {
      observer.observe(heading);
    });
  }

  function updateScrollState() {
    var scrollable = document.documentElement.scrollHeight - window.innerHeight;
    var ratio = scrollable > 0 ? window.scrollY / scrollable : 0;
    progress.style.width = Math.max(0, Math.min(100, ratio * 100)) + "%";
    backToTop.dataset.visible = window.scrollY > 900 ? "true" : "false";
  }
  window.addEventListener("scroll", updateScrollState, { passive: true });
  updateScrollState();

  var definedTermSetId = absoluteFragment("math-wiki");
  var graph = [
    {
      "@id": definedTermSetId,
      "@type": "DefinedTermSet",
      name: "The Math Behind the Glossary",
      description:
        "A hypermedia knowledge graph for language models, reasoning, self-play, inference, and evaluation.",
      hasDefinedTerm: entries.map(function (entry) {
        return { "@id": absoluteFragment(entry.id) };
      })
    }
  ];

  entries.forEach(function (entry) {
    var nav = entry.heading.nextElementSibling;
    var up = nav && nav.querySelector('[rel~="up"]');
    var related = nav && nav.querySelector('[rel~="related"]');
    graph.push({
      "@id": absoluteFragment(entry.id),
      "@type": "DefinedTerm",
      name: entry.title,
      inDefinedTermSet: { "@id": definedTermSetId },
      isPartOf: up ? { "@id": new URL(up.href, window.location.href).href } : undefined,
      relatedLink: related
        ? [new URL(related.href, window.location.href).href]
        : undefined,
      subjectOf: equations
        .filter(function (equation) {
          return equation.dataset.concept === "#" + entry.id;
        })
        .map(function (equation) {
          return { "@id": absoluteFragment(equation.id) };
        })
    });
  });

  equations.forEach(function (equation) {
    graph.push({
      "@id": absoluteFragment(equation.id),
      "@type": "LearningResource",
      learningResourceType: "Equation",
      isPartOf: {
        "@id": absoluteFragment(equation.dataset.concept.replace(/^#/, ""))
      },
      position: Number(equation.dataset.equationIndex)
    });
  });

  var linkedData = document.createElement("script");
  linkedData.id = "math-wiki-linked-data";
  linkedData.type = "application/ld+json";
  linkedData.textContent = JSON.stringify(
    { "@context": "https://schema.org", "@graph": graph },
    null,
    2
  );
  document.head.appendChild(linkedData);

  if (window.location.hash) {
    window.requestAnimationFrame(function () {
      var target = document.getElementById(window.location.hash.slice(1));
      if (target) target.scrollIntoView();
    });
  }
})();
