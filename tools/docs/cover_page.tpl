{#- Cover page of the printed manual (mkdocs-print-site-plugin).
    Variables: config (mkdocs.yml). Styled by docs/assets/stylesheets/print.css. -#}
<div class="dps-cover">
  <p class="dps-cover-kicker">Your stepper, seen as a smart servo</p>
  <h1 class="dps-cover-title">{{ config.site_name }}</h1>
  <p class="dps-cover-subtitle">Firmware, development environment, host tools and bring-up</p>
  <p class="dps-cover-lead">{{ config.site_description }}</p>

  <table class="dps-cover-meta">
    <tr><th>Document version</th><td>{{ config.extra.doc_version }}</td></tr>
    {%- if config.extra.doc_date %}
    <tr><th>Date</th><td>{{ config.extra.doc_date }}</td></tr>
    {%- endif %}
    <tr><th>Target</th><td>STMicroelectronics B-G431B-ESC1 (STM32G431CB), Zephyr RTOS 4.5</td></tr>
    <tr><th>Protocol</th><td>DYNAMIXEL Protocol 2.0 (XM430-W350), 1 Mbps</td></tr>
    {%- if config.site_author %}
    <tr><th>Author</th><td>{{ config.site_author }}</td></tr>
    {%- endif %}
    {%- if config.repo_url %}
    <tr><th>Repository</th><td>{{ config.repo_url }}</td></tr>
    {%- endif %}
  </table>
</div>
